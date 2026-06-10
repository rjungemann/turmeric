/* emit_module.c -- program/module assembly (emit_program, emit_header, emit_implementation). */
#include "emit_internal.h"
#include "emit_cps.h"   /* cps-transform-plan: DK substrate prelude + wiring */
#include "globals.h"   /* Phase I: g_emit_abi_trace */

/* ------------ program-level emit ------------ */

/* file-scope-inline-c-dedup: a top-level ```c ... ``` block lowers to file-scope
 * C (typedefs, struct tags, helper fns).  When several modules linked into one
 * TU each carry an *equivalent* such block -- the documented "redeclare the
 * carrier struct in every module that touches its fields" idiom (see the httpd
 * / tourist spices) -- emitting every copy verbatim at file scope is a C
 * redefinition error (`redefinition of 'struct __foo'`).  De-duplicate so each
 * distinct block is emitted exactly once per TU.
 *
 * The comparison is *whitespace-insensitive*: the same struct declared in two
 * different module files almost never matches byte-for-byte (indentation and
 * line-breaks differ across hand-written files), so a raw memcmp would let the
 * reformatted-but-identical copies collide anyway.  We compare a normalized key
 * (every run of whitespace collapsed to a single space, ends trimmed) instead.
 * Two blocks that genuinely differ in *content* (different struct tag, fields,
 * field order, or types -- anything beyond whitespace) produce different keys,
 * are NOT de-duplicated, and still reach cc as a real `redefinition` error, so
 * a genuine layout disagreement is never silently masked. */
typedef struct {
    char     **keys;   /* owned, whitespace-normalized text of each kept block */
    uint32_t   n;
    uint32_t   cap;
} InlineCDedup;

/* Collapse every run of ASCII whitespace to a single space and trim both ends,
 * returning a freshly malloc'd NUL-terminated string.  This is the dedup key:
 * indentation/line-break differences between two copies of the same declaration
 * normalize away, while any token difference survives. */
static char *inline_c_normalize_ws(const char *p, size_t len) {
    char *out = (char *)malloc(len + 1);
    size_t w = 0;
    bool pending_space = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            if (w > 0) pending_space = true;  /* defer; trims leading + trailing */
            continue;
        }
        if (pending_space) { out[w++] = ' '; pending_space = false; }
        out[w++] = (char)c;
    }
    out[w] = '\0';
    return out;
}

/* Return true if a whitespace-equivalent block was already recorded; otherwise
 * record this one and return false.  Callers emit the block only when false. */
static bool inline_c_dedup_seen(InlineCDedup *d, const char *p, size_t len) {
    char *key = inline_c_normalize_ws(p, len);
    for (uint32_t i = 0; i < d->n; i++) {
        if (strcmp(d->keys[i], key) == 0) { free(key); return true; }
    }
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->keys = (char **)realloc(d->keys, d->cap * sizeof(*d->keys));
    }
    d->keys[d->n++] = key;  /* ownership transferred to the dedup */
    return false;
}

static void inline_c_dedup_free(InlineCDedup *d) {
    for (uint32_t i = 0; i < d->n; i++) free(d->keys[i]);
    free(d->keys);
    d->keys = NULL;
    d->n = d->cap = 0;
}

static bool thunk_type_has_concrete_c_abi(Type t) {
    switch (t.kind) {
        case TY_NIL:
        case TY_BOOL:
        case TY_INT:
        case TY_FLOAT:
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_REF:
        case TY_LREF:
        case TY_RC:
        case TY_WEAK:
        case TY_REF_IMMUT:
        case TY_REF_MUT:
        case TY_EXCEPTION:
        case TY_CONT:
        case TY_CLONEABLE_CONT:
        case TY_NEVER:
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
        case TY_SET:
        case TY_HANDLER:
        case TY_SESSION:
        case TY_ROLE:
        case TY_GENERATOR:
            return true;
        case TY_STRUCT:
            return t.as.struct_.def && !t.as.struct_.def->is_opaque;
        case TY_ADT:
            return t.as.adt_.def != NULL;
        default:
            return false;
    }
}

bool use_typed_thunk_abi(Type result_type, Type *param_types, uint8_t n_params) {
    if (!thunk_type_has_concrete_c_abi(result_type)) return false;
    for (uint8_t i = 0; i < n_params; i++) {
        if (!thunk_type_has_concrete_c_abi(param_types[i])) return false;
    }
    return true;
}

static void append_sanitized_c_token(Buf *out, const char *raw) {
    if (!raw || !*raw) {
        buf_puts(out, "anon");
        return;
    }
    for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
        buf_putc(out, isalnum(*p) ? (char)*p : '_');
    }
}

static char *typed_thunk_typedef_name(Type result_type, Type *param_types, uint8_t n_params) {
    Buf name;
    buf_init(&name);
    buf_puts(&name, "tur_thunk_");
    append_sanitized_c_token(&name, type_c_name(result_type));
    for (uint8_t i = 0; i < n_params; i++) {
        buf_putc(&name, '_');
        append_sanitized_c_token(&name, type_c_name(param_types[i]));
    }
    buf_puts(&name, "_t");
    buf_putc(&name, '\0');
    char *result = strdup(name.data);
    buf_free(&name);
    if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
    return result;
}

char *ensure_typed_thunk_typedef(EmitCtx *ctx, Buf *out,
                                 Type result_type, Type *param_types, uint8_t n_params) {
    if (!use_typed_thunk_abi(result_type, param_types, n_params)) return NULL;

    char *name = typed_thunk_typedef_name(result_type, param_types, n_params);
    for (uint32_t i = 0; i < ctx->n_thunk_typedef_names; i++) {
        if (strcmp(ctx->thunk_typedef_names[i], name) == 0) {
            return name;
        }
    }

    if (ctx->n_thunk_typedef_names >= ctx->cap_thunk_typedef_names) {
        uint32_t new_cap = ctx->cap_thunk_typedef_names ? ctx->cap_thunk_typedef_names * 2 : 8;
        char **new_names = (char **)realloc(ctx->thunk_typedef_names, new_cap * sizeof(char *));
        if (!new_names) { fprintf(stderr, "tur: oom\n"); abort(); }
        ctx->thunk_typedef_names = new_names;
        ctx->cap_thunk_typedef_names = new_cap;
    }
    ctx->thunk_typedef_names[ctx->n_thunk_typedef_names++] = strdup(name);
    if (!ctx->thunk_typedef_names[ctx->n_thunk_typedef_names - 1]) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }

    Buf *target = ctx->thunk_typedefs ? ctx->thunk_typedefs : out;
    buf_printf(target, "typedef %s (*%s)(void *", type_c_name(result_type), name);
    for (uint8_t i = 0; i < n_params; i++) {
        buf_printf(target, ", %s", type_c_name(param_types[i]));
    }
    buf_puts(target, ");\n");
    return name;
}

static char *typed_fatshim_name(Type result_type, Type *param_types, uint8_t n_params) {
    Buf name;
    buf_init(&name);
    buf_puts(&name, "__tur_fatshim_");
    append_sanitized_c_token(&name, type_c_name(result_type));
    for (uint8_t i = 0; i < n_params; i++) {
        buf_putc(&name, '_');
        append_sanitized_c_token(&name, type_c_name(param_types[i]));
    }
    buf_putc(&name, '\0');
    char *result = strdup(name.data);
    buf_free(&name);
    if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
    return result;
}

char *ensure_typed_fatshim(EmitCtx *ctx,
                           Type result_type, Type *param_types, uint8_t n_params) {
    /* The call site invokes the boxed fn through the typed-thunk cast only when
     * use_typed_thunk_abi holds; otherwise it falls back to the int64_t fat-call
     * path that the preamble __tur_fatshim<arity> shim already satisfies. */
    if (!use_typed_thunk_abi(result_type, param_types, n_params)) return NULL;
    /* All-int64_t carrier signatures are likewise served by the preamble shim
     * (its int64_t (*)(void *, int64_t...) ABI equals the typed-thunk cast),
     * so emit nothing new and keep int64 fixtures churn-free. */
    bool all_int64 = strcmp(type_c_name(result_type), "int64_t") == 0;
    for (uint8_t i = 0; all_int64 && i < n_params; i++) {
        if (strcmp(type_c_name(param_types[i]), "int64_t") != 0) all_int64 = false;
    }
    if (all_int64) return NULL;

    char *name = typed_fatshim_name(result_type, param_types, n_params);
    for (uint32_t i = 0; i < ctx->n_fatshim_names; i++) {
        if (strcmp(ctx->fatshim_names[i], name) == 0) return name;
    }
    if (ctx->n_fatshim_names >= ctx->cap_fatshim_names) {
        uint32_t new_cap = ctx->cap_fatshim_names ? ctx->cap_fatshim_names * 2 : 8;
        char **nn = (char **)realloc(ctx->fatshim_names, new_cap * sizeof(char *));
        if (!nn) { fprintf(stderr, "tur: oom\n"); abort(); }
        ctx->fatshim_names = nn;
        ctx->cap_fatshim_names = new_cap;
    }
    ctx->fatshim_names[ctx->n_fatshim_names++] = strdup(name);
    if (!ctx->fatshim_names[ctx->n_fatshim_names - 1]) { fprintf(stderr, "tur: oom\n"); abort(); }

    /* static R name(void *__e, A0 a0, ...) {
     *     return ((R (*)(A0, ...))(intptr_t)((int64_t *)__e)[1])(a0, ...);
     * }
     * Slot 1 of the fat box holds the original bare fn pointer (EX_FN_TO_FAT);
     * slot 0 holds this shim, invoked through the typed-thunk cast at the call
     * site.  The -Wunused-function pragma in the preamble covers shims a TU
     * boxes but never reaches. */
    Buf *target = ctx->thunk_typedefs ? ctx->thunk_typedefs : ctx->file;
    bool has_ret = result_type.kind != TY_NIL && result_type.kind != TY_NEVER;
    buf_printf(target, "static %s %s(void *__e", type_c_name(result_type), name);
    for (uint8_t i = 0; i < n_params; i++) {
        buf_printf(target, ", %s a%u", type_c_name(param_types[i]), (unsigned)i);
    }
    buf_puts(target, ") {\n    ");
    if (has_ret) buf_puts(target, "return ");
    buf_printf(target, "((%s (*)(", type_c_name(result_type));
    if (n_params == 0) {
        buf_puts(target, "void");
    } else {
        for (uint8_t i = 0; i < n_params; i++) {
            if (i) buf_puts(target, ", ");
            buf_puts(target, type_c_name(param_types[i]));
        }
    }
    buf_puts(target, "))(intptr_t)((int64_t *)__e)[1])(");
    for (uint8_t i = 0; i < n_params; i++) {
        if (i) buf_puts(target, ", ");
        buf_printf(target, "a%u", (unsigned)i);
    }
    buf_puts(target, ");\n}\n");
    return name;
}

static char *typed_poly_to_fat_name(Type result_type, const Type *arg_types,
                                    uint32_t n_args) {
    Buf name;
    buf_init(&name);
    /* The arity tag mirrors __tur_poly_to_fat<N>; keep "1" backward-compatible
     * for the unary family so existing fixtures stay churn-free. */
    buf_printf(&name, "__tur_poly_to_fat%u_", (unsigned)n_args);
    append_sanitized_c_token(&name, type_c_name(result_type));
    for (uint32_t i = 0; i < n_args; i++) {
        buf_putc(&name, '_');
        append_sanitized_c_token(&name, type_c_name(arg_types[i]));
    }
    buf_putc(&name, '\0');
    char *result = strdup(name.data);
    buf_free(&name);
    if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
    return result;
}

char *ensure_typed_poly_to_fat(EmitCtx *ctx, Type result_type,
                               const Type *arg_types, uint32_t n_args) {
    /* The sink invokes the boxed handle through the typed-thunk cast only when
     * use_typed_thunk_abi holds for (R, A0..An); otherwise it falls back to the
     * int64_t carrier that the preamble __tur_poly_to_fat<N> shim already
     * satisfies. */
    if (!use_typed_thunk_abi(result_type, (Type *)arg_types, (uint8_t)n_args)) return NULL;
    /* All-int64_t carrier signatures are likewise served by the preamble shim
     * (its int64_t (*)(void *, int64_t...) ABI equals the typed-thunk cast), so
     * emit nothing new and keep int64/pointer poly boxes churn-free. */
    bool all_int64 = strcmp(type_c_name(result_type), "int64_t") == 0;
    for (uint32_t i = 0; all_int64 && i < n_args; i++) {
        if (strcmp(type_c_name(arg_types[i]), "int64_t") != 0) all_int64 = false;
    }
    if (all_int64) return NULL;

    char *name = typed_poly_to_fat_name(result_type, arg_types, n_args);
    for (uint32_t i = 0; i < ctx->n_poly_fatshim_names; i++) {
        if (strcmp(ctx->poly_fatshim_names[i], name) == 0) return name;
    }
    if (ctx->n_poly_fatshim_names >= ctx->cap_poly_fatshim_names) {
        uint32_t new_cap = ctx->cap_poly_fatshim_names ? ctx->cap_poly_fatshim_names * 2 : 8;
        char **nn = (char **)realloc(ctx->poly_fatshim_names, new_cap * sizeof(char *));
        if (!nn) { fprintf(stderr, "tur: oom\n"); abort(); }
        ctx->poly_fatshim_names = nn;
        ctx->cap_poly_fatshim_names = new_cap;
    }
    ctx->poly_fatshim_names[ctx->n_poly_fatshim_names++] = strdup(name);
    if (!ctx->poly_fatshim_names[ctx->n_poly_fatshim_names - 1]) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }

    /* static R name(void *__e, A0 a0, ...) {
     *     int64_t *__b = (int64_t *)__e;
     *     return ((R (*)(void *, A0, ...))(intptr_t)__b[1])((void *)(intptr_t)__b[2], a0, ...);
     * }
     * Slot 1 holds the method's real (typed) N-ary fn pointer; slot 2 its env.
     * The carrier erases the signature to int64_t (*)(void *, int64_t...); this
     * shim re-types it back to the true R (*)(void *, A0..An) so the sink's
     * typed-thunk cast at slot 0 matches the actual ABI and every argument is
     * forwarded. */
    Buf *target = ctx->thunk_typedefs ? ctx->thunk_typedefs : ctx->file;
    const char *rc = type_c_name(result_type);
    bool has_ret = result_type.kind != TY_NIL && result_type.kind != TY_NEVER;
    buf_printf(target, "static %s %s(void *__e", rc, name);
    for (uint32_t i = 0; i < n_args; i++) {
        buf_printf(target, ", %s a%u", type_c_name(arg_types[i]), (unsigned)i);
    }
    buf_puts(target, ") {\n    int64_t *__b = (int64_t *)__e;\n    ");
    if (has_ret) buf_puts(target, "return ");
    buf_printf(target, "((%s (*)(void *", rc);
    for (uint32_t i = 0; i < n_args; i++) buf_printf(target, ", %s", type_c_name(arg_types[i]));
    buf_puts(target, "))(intptr_t)__b[1])((void *)(intptr_t)__b[2]");
    for (uint32_t i = 0; i < n_args; i++) buf_printf(target, ", a%u", (unsigned)i);
    buf_puts(target, ");\n}\n");
    return name;
}

static bool emit_abi_type_has_named_tyvar(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name != NULL;
        case TY_APP:
            return emit_abi_type_has_named_tyvar(t->as.app.fn) ||
                   emit_abi_type_has_named_tyvar(t->as.app.arg);
        case TY_UNION:
            for (uint8_t i = 0; i < t->as.union_.n_members; i++) {
                if (emit_abi_type_has_named_tyvar(t->as.union_.members[i])) return true;
            }
            return false;
        case TY_INTERSECTION:
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++) {
                if (emit_abi_type_has_named_tyvar(t->as.intersection_.members[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

/* GS5/CS3: emit_abi_collect_type_bindings / emit_abi_find_type_binding used to
 * live here as a duplicate of elab_call.c's substitution machinery. They have
 * been removed -- elab now attaches the substitution to EX_CALL via
 * call_.abi_bindings, and emit_abi_register_call consumes that directly. */

static bool emit_abi_find_type_binding(const AbiTypeBinding *bindings, uint8_t n_bindings,
                                       const char *name, uint8_t *out_idx) {
    if (!name) return false;
    for (uint8_t i = 0; i < n_bindings; i++) {
        if (bindings[i].name && strcmp(bindings[i].name, name) == 0) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

/* ASan/LSan plan (Option C): same arena-backed scratch policy as
 * emit_type_scratch in emit_core.c. The instantiated Type nodes feed the
 * EmitAbiSpecialization records, which live for the whole emit pass; the
 * arena is bulk-freed when the pass finishes. NULL arena falls back to malloc
 * (process-lifetime), matching the prior behavior. */
static void *emit_abi_type_scratch(Arena *arena, size_t size) {
    if (arena) return arena_alloc(arena, size);
    void *p = malloc(size);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    return p;
}

static Type emit_abi_instantiate_type(const Type *t,
                                      const AbiTypeBinding *bindings, uint8_t n_bindings,
                                      Arena *arena) {
    if (!t) return emit_type_from_kind(TY_UNKNOWN);
    switch (t->kind) {
        case TY_TYVAR: {
            uint8_t idx = 0;
            if (t->as.tyvar_.name &&
                emit_abi_find_type_binding(bindings, n_bindings, t->as.tyvar_.name, &idx)) {
                return bindings[idx].type;
            }
            return *t;
        }
        case TY_APP: {
            if (!t->as.app.fn || !t->as.app.arg) return *t;
            Type fn = emit_abi_instantiate_type(t->as.app.fn, bindings, n_bindings, arena);
            Type arg = emit_abi_instantiate_type(t->as.app.arg, bindings, n_bindings, arena);
            Type out = *t;
            out.as.app.fn = (Type *)emit_abi_type_scratch(arena, sizeof(Type));
            out.as.app.arg = (Type *)emit_abi_type_scratch(arena, sizeof(Type));
            *out.as.app.fn = fn;
            *out.as.app.arg = arg;
            return out;
        }
        case TY_UNION: {
            Type out = *t;
            if (t->as.union_.n_members == 0 || !t->as.union_.members) return out;
            Type **members = (Type **)emit_abi_type_scratch(arena, t->as.union_.n_members * sizeof(Type *));
            out.as.union_.members = members;
            for (uint8_t i = 0; i < t->as.union_.n_members; i++) {
                members[i] = (Type *)emit_abi_type_scratch(arena, sizeof(Type));
                *members[i] = emit_abi_instantiate_type(t->as.union_.members[i], bindings, n_bindings, arena);
            }
            return out;
        }
        case TY_INTERSECTION: {
            Type out = *t;
            if (t->as.intersection_.n_members == 0 || !t->as.intersection_.members) return out;
            Type **members = (Type **)emit_abi_type_scratch(arena, t->as.intersection_.n_members * sizeof(Type *));
            out.as.intersection_.members = members;
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++) {
                members[i] = (Type *)emit_abi_type_scratch(arena, sizeof(Type));
                *members[i] = emit_abi_instantiate_type(t->as.intersection_.members[i], bindings, n_bindings, arena);
            }
            return out;
        }
        default:
            return *t;
    }
}

static const Expr *emit_abi_find_fn_expr(const Expr **items, uint32_t n_items, const Binding *binding) {
    for (uint32_t i = 0; i < n_items; i++) {
        if (items[i]->kind == EX_FN_DEF && items[i]->as.fn_def_.fn &&
            items[i]->as.fn_def_.fn->binding == binding) {
            return items[i];
        }
    }
    return NULL;
}

static char *emit_abi_clone_name(const Binding *binding, Type result_type, Type *arg_types, uint8_t n_args) {
    Buf name;
    buf_init(&name);
    append_sanitized_c_token(&name, binding && binding->name ? binding->name->name : "fn");
    buf_puts(&name, "__spec__");
    append_sanitized_c_token(&name, type_c_name(result_type));
    for (uint8_t i = 0; i < n_args; i++) {
        buf_putc(&name, '_');
        append_sanitized_c_token(&name, type_c_name(arg_types[i]));
    }
    buf_putc(&name, '\0');
    char *result = strdup(name.data);
    buf_free(&name);
    if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
    return result;
}

/* GHE2: find an existing ABI specialization matching (fn_binding, arg_types,
 * result_type), else append a fresh one.  Shared by the call path
 * (emit_abi_register_call) and the fn-value path (emit_abi_scan_fn_values), so
 * a generic function referenced *as a value* gets the same per-K clone a direct
 * call would.  Does not record a specialized-call mapping -- the caller decides
 * whether this spec stands in for a call site (call path) or a value reference
 * (fn-value path, resolved later in atom_var). */
static EmitAbiSpecialization *emit_abi_intern_spec(
        EmitCtx *ctx, Binding *fn_binding, const Expr *fn_expr, FnDef *fd,
        const AbiTypeBinding *bindings, uint8_t n_bindings,
        const Type *arg_types, uint8_t n_spec_args, Type result_type,
        const Expr *call_expr) {
    for (uint32_t i = 0; i < ctx->n_abi_specializations; i++) {
        EmitAbiSpecialization *spec = &ctx->abi_specializations[i];
        if (spec->binding != fn_binding || spec->n_args != n_spec_args ||
            !type_eq(spec->result_type, result_type)) {
            continue;
        }
        bool args_match = true;
        for (uint8_t ai = 0; ai < n_spec_args; ai++) {
            if (!type_eq(spec->arg_types[ai], arg_types[ai])) {
                args_match = false;
                break;
            }
        }
        if (args_match) return spec;
    }

    if (ctx->n_abi_specializations >= ctx->cap_abi_specializations) {
        uint32_t new_cap = ctx->cap_abi_specializations ? ctx->cap_abi_specializations * 2 : 8;
        EmitAbiSpecialization *new_specs = (EmitAbiSpecialization *)realloc(
            ctx->abi_specializations, new_cap * sizeof(EmitAbiSpecialization));
        if (!new_specs) { fprintf(stderr, "tur: oom\n"); abort(); }
        ctx->abi_specializations = new_specs;
        ctx->cap_abi_specializations = new_cap;
    }

    EmitAbiSpecialization *spec = &ctx->abi_specializations[ctx->n_abi_specializations++];
    memset(spec, 0, sizeof(*spec));
    spec->inner_closure_spec_idx = -1;
    spec->call_expr = call_expr;
    spec->fn_expr = fn_expr;  /* NULL for borrow specs */
    spec->fn = fd;            /* NULL for borrow specs */
    spec->binding = fn_binding;
    spec->n_bindings = n_bindings;
    for (uint8_t i = 0; i < n_bindings; i++) spec->bindings[i] = bindings[i];
    spec->n_args = n_spec_args;
    for (uint8_t i = 0; i < n_spec_args; i++) spec->arg_types[i] = arg_types[i];
    spec->result_type = result_type;
    spec->clone_name = emit_abi_clone_name(fn_binding, result_type, spec->arg_types, n_spec_args);
    /* external_linkage is set later by the caller (emit_implementation) for
     * separate-compilation builds; left false here so whole-program builds
     * continue to emit static clones. */
    return spec;
}

/* KB-022: record that `binding` is the target of a direct (carrier) call, so a
 * generic-unsafe definition for it is still emitted as a fallback. */
static void emit_abi_note_carrier_call(EmitCtx *ctx, const Binding *binding) {
    if (!ctx || !binding) return;
    for (uint32_t i = 0; i < ctx->n_carrier_call_bindings; i++) {
        if (ctx->carrier_call_bindings[i] == binding) return;
    }
    if (ctx->n_carrier_call_bindings >= ctx->cap_carrier_call_bindings) {
        uint32_t new_cap = ctx->cap_carrier_call_bindings
                           ? ctx->cap_carrier_call_bindings * 2 : 8;
        const Binding **grown = (const Binding **)realloc(ctx->carrier_call_bindings,
            new_cap * sizeof(const Binding *));
        if (!grown) { fprintf(stderr, "tur: oom\n"); abort(); }
        ctx->carrier_call_bindings = grown;
        ctx->cap_carrier_call_bindings = new_cap;
    }
    ctx->carrier_call_bindings[ctx->n_carrier_call_bindings++] = binding;
}

static void emit_abi_record_specialized_call(EmitCtx *ctx, const Expr *call, const char *clone_name) {
    if (ctx->n_specialized_calls >= ctx->cap_specialized_calls) {
        uint32_t new_cap = ctx->cap_specialized_calls ? ctx->cap_specialized_calls * 2 : 8;
        const Expr **new_exprs = (const Expr **)realloc(ctx->specialized_call_exprs,
            new_cap * sizeof(Expr *));
        const char **new_names = (const char **)realloc(ctx->specialized_call_names,
            new_cap * sizeof(char *));
        if (!new_exprs || !new_names) { fprintf(stderr, "tur: oom\n"); abort(); }
        ctx->specialized_call_exprs = new_exprs;
        ctx->specialized_call_names = new_names;
        ctx->cap_specialized_calls = new_cap;
    }
    ctx->specialized_call_exprs[ctx->n_specialized_calls] = call;
    ctx->specialized_call_names[ctx->n_specialized_calls] = clone_name;
    ctx->n_specialized_calls++;
}

/* GHE2: the call/spec scan recurses into freshly-created spec bodies. */
static void emit_abi_scan_expr(EmitCtx *ctx, const Expr *e,
                               const Expr **items, uint32_t n_items);

static bool emit_abi_fn_is_generic_unsafe(const Expr *e);

/* Variant 2 (generic-struct-opaque-element): detect a generic-*relay* call that
 * must NOT be carrier-noted.  Such a call sits inside a generic body (no active
 * specialization) and still has an abstract binding -- it maps the callee's
 * tyvars to the enclosing generic's tyvars rather than to concrete types.  When
 * the callee is generic-unsafe (a by-value aggregate arg/result whose carrier
 * body is invalid C, e.g. `recv : (Pair T ptr<void>)`), carrier-noting it would
 * force that broken generic body to be emitted.  Instead the call is resolved by
 * binding composition (in emit_abi_register_call) when the enclosing generic is
 * specialized, so the inner callee specializes too.  Carrier-safe relays (whose
 * generic carrier body *is* valid C, e.g. a fully-generic `equal-cong`) are left
 * untouched so KB-022's carrier fallback still emits them. */
static bool emit_abi_call_is_generic_relay(const EmitCtx *ctx, const Expr *call,
                                           const Expr **items, uint32_t n_items) {
    if (!ctx || ctx->current_abi_specialization) return false;
    if (!call || call->kind != EX_CALL) return false;
    /* The call must sit inside a *generic* function body: only then will the
     * enclosing fn be specialized and the relay's abstract bindings composed to
     * concrete ones.  A call in a monomorphic body (e.g. `map_count(map_new())`
     * in `main`, KB-022) with abstract bindings is an unresolvable phantom that
     * still needs its carrier fallback -- do not treat it as a relay. */
    if (!emit_abi_fn_is_generic_unsafe(ctx->current_scan_fn)) return false;
    const AbiTypeBinding *b = call->as.call_.abi_bindings;
    uint8_t n = call->as.call_.n_abi_bindings;
    if (!b || n == 0) return false;
    bool abstract = false;
    for (uint8_t i = 0; i < n; i++) {
        if (emit_abi_type_has_named_tyvar(&b[i].type)) { abstract = true; break; }
    }
    if (!abstract) return false;
    Binding *fb = call->as.call_.fn_binding;
    if (!fb) return false;
    const Expr *fe = emit_abi_find_fn_expr(items, n_items, fb);
    return fe && emit_abi_fn_is_generic_unsafe(fe);
}

/* GDE1: scan an expression subtree for a typeclass-method dispatch call where
 * the receiver is a TY_TYVAR bound to a TY_APP type in `bindings`.  Returns
 * true when any such call is found.  This detects the case where the C ABI is
 * unchanged (both carrier and concrete type are int64_t) but the instance
 * dispatched through the dict must still be re-resolved after monomorphization
 * (e.g. an eq? call on a Map[cstr int] argument -- same ABI as int, but needs
 * __inst_Eq_eq__Map not __inst_Eq_eq__int). */
static bool body_has_dispatch_on_app_tyvar(
        const Expr *e,
        const AbiTypeBinding *bindings, uint8_t n_bindings) {
    if (!e) return false;
    if (e->kind == EX_CALL && e->as.call_.dict_arg && e->as.call_.n_args >= 1) {
        const Expr *recv = e->as.call_.args[0];
        while (recv && recv->kind == EX_ASCRIBE)
            recv = recv->as.ascribe_.inner;
        if (recv && recv->type.kind == TY_TYVAR && recv->type.as.tyvar_.name) {
            for (uint8_t i = 0; i < n_bindings; i++) {
                if (bindings[i].name &&
                    strcmp(bindings[i].name, recv->type.as.tyvar_.name) == 0 &&
                    bindings[i].type.kind == TY_APP) {
                    return true;
                }
            }
        }
    }
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                if (body_has_dispatch_on_app_tyvar(e->as.program.items[i], bindings, n_bindings))
                    return true;
            break;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn)
                return body_has_dispatch_on_app_tyvar(e->as.fn_def_.fn->body, bindings, n_bindings);
            break;
        case EX_DEF:
            return body_has_dispatch_on_app_tyvar(e->as.def_.init, bindings, n_bindings);
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (body_has_dispatch_on_app_tyvar(e->as.let_.bindings[i].init, bindings, n_bindings))
                    return true;
            return body_has_dispatch_on_app_tyvar(e->as.let_.body, bindings, n_bindings);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (body_has_dispatch_on_app_tyvar(e->as.do_.items[i], bindings, n_bindings))
                    return true;
            break;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (body_has_dispatch_on_app_tyvar(e->as.builtin.args[i], bindings, n_bindings))
                    return true;
            break;
        case EX_IF:
            return body_has_dispatch_on_app_tyvar(e->as.if_.cond, bindings, n_bindings) ||
                   body_has_dispatch_on_app_tyvar(e->as.if_.then_, bindings, n_bindings) ||
                   body_has_dispatch_on_app_tyvar(e->as.if_.else_or_null, bindings, n_bindings);
        case EX_WHILE:
            return body_has_dispatch_on_app_tyvar(e->as.while_.cond, bindings, n_bindings) ||
                   body_has_dispatch_on_app_tyvar(e->as.while_.body, bindings, n_bindings);
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (body_has_dispatch_on_app_tyvar(e->as.call_.args[i], bindings, n_bindings))
                    return true;
            return body_has_dispatch_on_app_tyvar(e->as.call_.fn_expr, bindings, n_bindings);
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (body_has_dispatch_on_app_tyvar(e->as.make_struct_.field_values[i], bindings, n_bindings))
                    return true;
            break;
        case EX_GET_FIELD:
            return body_has_dispatch_on_app_tyvar(e->as.get_field_.struct_expr, bindings, n_bindings);
        case EX_SET_FIELD:
            return body_has_dispatch_on_app_tyvar(e->as.set_field_.receiver, bindings, n_bindings) ||
                   body_has_dispatch_on_app_tyvar(e->as.set_field_.value, bindings, n_bindings);
        case EX_RETURN:
            return body_has_dispatch_on_app_tyvar(e->as.return_.value, bindings, n_bindings);
        case EX_ASCRIBE:
            return body_has_dispatch_on_app_tyvar(e->as.ascribe_.inner, bindings, n_bindings);
        case EX_CAST:
            return body_has_dispatch_on_app_tyvar(e->as.cast_.expr, bindings, n_bindings);
        case EX_REINTERPRET:
            return body_has_dispatch_on_app_tyvar(e->as.reinterpret_.expr, bindings, n_bindings);
        default:
            break;
    }
    return false;
}

/* poly-closure-result-specialization: a float-class type lives in a different
 * register class (xmm0) than the int64 carrier (rax), so a closure body shared
 * across monomorphizations miscompiles when its result/param/captured tyvar
 * resolves to a float.  These helpers decide whether a generic
 * closure-returning defn needs a register-class-correct inner-body clone. */
static bool abi_kind_is_float(TypeKind k) {
    return k == TY_FLOAT || k == TY_FLOAT32 || k == TY_FLOAT64;
}

/* True when type `t` is (at the top level) a tyvar that binds to a float-class
 * type under `bindings`. */
static bool abi_type_binds_to_float(const Type *t, const AbiTypeBinding *bindings,
                                    uint8_t n_bindings) {
    if (!t || t->kind != TY_TYVAR || !t->as.tyvar_.name) return false;
    for (uint8_t i = 0; i < n_bindings; i++) {
        if (bindings[i].name && strcmp(bindings[i].name, t->as.tyvar_.name) == 0)
            return abi_kind_is_float(bindings[i].type.kind);
    }
    return false;
}

/* True when the inner closure a generic defn returns has a result or argument
 * tyvar that resolves to a float under `bindings` -- i.e. its int64-carrier
 * thunk ABI would be a register-class miscompile and it needs a per-spec
 * clone. */
/* Mint a fresh Symbol in `arena` for a generated C identifier.  env-struct
 * dedup compares by pointer identity, so a fresh Symbol is guaranteed distinct
 * from the base closure's env_name -- exactly what an inner-body spec needs so
 * its float layout does not collide with the base int64-carrier struct. */
static const Symbol *emit_arena_symbol(Arena *arena, const char *s) {
    size_t n = strlen(s);
    char *buf = (char *)arena_alloc(arena, n + 1);
    memcpy(buf, s, n + 1);
    Symbol *sym = (Symbol *)arena_alloc(arena, sizeof(Symbol));
    sym->name = buf;
    sym->len = (uint32_t)n;
    sym->hash = 0;
    return sym;
}

static bool emit_inner_closure_needs_float_spec(Binding *inner,
        const AbiTypeBinding *bindings, uint8_t n_bindings) {
    if (!inner || inner->type.kind != TY_FN) return false;
    if (abi_type_binds_to_float(inner->type.as.fn.result_full_type, bindings, n_bindings))
        return true;
    for (uint8_t i = 0; i < inner->type.as.fn.arity; i++) {
        const Type *at = inner->type.as.fn.arg_full_types
            ? inner->type.as.fn.arg_full_types[i] : NULL;
        if (abi_type_binds_to_float(at, bindings, n_bindings)) return true;
    }
    return false;
}

static void emit_abi_register_call(EmitCtx *ctx, const Expr *call,
                                   const Expr **items, uint32_t n_items,
                                   const Type *result_type_override) {
    if (!call || call->kind != EX_CALL || !call->as.call_.fn_binding) return;
    /* GS5/CS3: elab attaches the named-tyvar substitution to the call when it
     * matters; absence of bindings means there is nothing to specialize. */
    const AbiTypeBinding *bindings = call->as.call_.abi_bindings;
    uint8_t n_bindings = call->as.call_.n_abi_bindings;
    if (!bindings || n_bindings == 0) return;

    /* Variant 2 of generic-struct-opaque-element: when this call is scanned
     * inside an *active* specialization, its abi_bindings (captured at elab
     * time) map this callee's tyvars to the *enclosing generic's* tyvars, which
     * are still abstract.  Compose them through the active spec's concrete
     * bindings so a generic-from-generic call (e.g. a forwarder `fwd` calling
     * `recv`, both `[T R]`) specializes to the concrete clone instead of
     * falling back to the broken carrier template. */
    AbiTypeBinding composed[ABI_TYPE_BINDINGS_MAX];
    if (ctx->current_abi_specialization &&
        ctx->current_abi_specialization->n_bindings > 0 &&
        n_bindings <= ABI_TYPE_BINDINGS_MAX) {
        bool changed = false;
        for (uint8_t i = 0; i < n_bindings; i++) {
            composed[i].name = bindings[i].name;
            composed[i].type = emit_abi_instantiate_type(
                &bindings[i].type,
                ctx->current_abi_specialization->bindings,
                ctx->current_abi_specialization->n_bindings,
                ctx->type_arena);
            if (strcmp(type_c_name(bindings[i].type),
                       type_c_name(composed[i].type)) != 0) {
                changed = true;
            }
        }
        if (changed) bindings = composed;
    }

    Binding *fn_binding = call->as.call_.fn_binding;
    if (call->as.call_.fn_expr || fn_binding->type.kind != TY_FN || !fn_binding->is_global ||
        fn_binding->closure_fn_binding) {
        return;
    }

    const Expr *fn_expr = emit_abi_find_fn_expr(items, n_items, fn_binding);
    /* J4: When fn_expr is NULL in separate-compilation mode, the generic is
     * defined in an imported module.  The caller (borrower) still rewrites its
     * call site to the clone name; the clone body is emitted by the owner. */
    bool borrow_path = false;
    FnDef *fd = NULL;
    if (!fn_expr) {
        if (!ctx->separate_compilation) return;
        borrow_path = true;
    } else if (!fn_expr->as.fn_def_.fn) {
        return;
    } else {
        fd = fn_expr->as.fn_def_.fn;
        if (fd->closure || !fd->body) return;
        /* Phase G: inline-C bodies only opt in to ABI specialization when they
         * contain __TUR_TY_<NAME>__ template markers. Without the template, the
         * body's C signature is hand-rolled (e.g. typedef'd as int64_t) and the
         * specialized parameter widths would mismatch the inline code; fall back
         * to the carrier path. */
        if (fd->body->kind == EX_INLINE_C &&
            !inline_c_has_ty_template(fd->body->as.inline_c_.inline_c)) {
            return;
        }
    }

    bool abi_changes = false;
    Type arg_types[MAX_FN_ARITY];
    uint8_t n_spec_args;

    if (!borrow_path) {
        /* Owned path: derive arg types from FnDef (same logic as before). */
        n_spec_args = fd->n_params;
        for (uint8_t i = 0; i < n_spec_args; i++) {
            const Type *expected_full = (fn_binding->type.as.fn.arg_full_types &&
                                         fn_binding->type.as.fn.arg_full_types[i])
                ? fn_binding->type.as.fn.arg_full_types[i]
                : &fd->params[i]->type;
            Type generic_arg = expected_full ? *expected_full : fd->param_types[i];
            arg_types[i] = expected_full
                ? emit_abi_instantiate_type(expected_full, bindings, n_bindings, ctx->type_arena)
                : fd->param_types[i];
            if (strcmp(type_c_name(generic_arg), type_c_name(arg_types[i])) != 0) {
                abi_changes = true;
            }
        }
    } else {
        /* J4: Borrow path: derive arg types from fn_binding's type info.
         * The generic's FnDef is not in this module's items; we use the
         * binding's full-type information (set by elab) to compute the same
         * clone name that the owning module will produce. */
        n_spec_args = fn_binding->type.as.fn.arity;
        for (uint8_t i = 0; i < n_spec_args; i++) {
            const Type *expected_full = fn_binding->type.as.fn.arg_full_types
                ? fn_binding->type.as.fn.arg_full_types[i] : NULL;
            if (expected_full) {
                Type generic_arg = *expected_full;
                arg_types[i] = emit_abi_instantiate_type(expected_full, bindings, n_bindings, ctx->type_arena);
                if (strcmp(type_c_name(generic_arg), type_c_name(arg_types[i])) != 0) {
                    abi_changes = true;
                }
            } else {
                /* Monomorphic arg -- no ABI change; use the concrete kind. */
                arg_types[i] = emit_type_from_kind(fn_binding->type.as.fn.arg_kinds[i]);
            }
        }
    }

    Type generic_result = fn_binding->type.as.fn.result_full_type
        ? *fn_binding->type.as.fn.result_full_type
        : emit_type_from_kind(fn_binding->type.as.fn.result_kind);
    Type result_type = result_type_override ? *result_type_override : call->type;
    /* Variant 2: inside an active specialization, `call->type` is still expressed
     * in the enclosing generic's tyvars (e.g. `(Pair T ptr<void>)`); instantiate
     * it through that spec's concrete bindings so a relayed aggregate result
     * specializes to `(Pair int ptr<void>)` rather than the int64_t carrier.  For
     * a concrete top-level call this is a no-op (no tyvars to substitute). */
    if (ctx->current_abi_specialization &&
        ctx->current_abi_specialization->n_bindings > 0) {
        result_type = emit_abi_instantiate_type(
            &result_type, ctx->current_abi_specialization->bindings,
            ctx->current_abi_specialization->n_bindings, ctx->type_arena);
    }
    if (strcmp(type_c_name(generic_result), type_c_name(result_type)) != 0) {
        abi_changes = true;
    }
    /* GDE1: even when the C ABI is unchanged, intern a specialization when the
     * body contains a typeclass-method dispatch on a TY_TYVAR receiver that is
     * bound to a TY_APP type (e.g. eq? on a Map[cstr int] argument).  Without
     * this, the base clone bakes the representative (int-carrier) instance and
     * the call never reaches the correct HKT instance (__inst_Eq_eq__Map). */
    bool instance_changes = false;
    if (!abi_changes && !borrow_path && fd && fd->body) {
        instance_changes = body_has_dispatch_on_app_tyvar(fd->body, bindings, n_bindings);
    }
    /* poly-closure-result-specialization (Stage B+C): when the callee returns a
     * lifted inner closure whose result/arg tyvar resolves to a float, the inner
     * body's int64-carrier thunk ABI is a register-class miscompile.  Force an
     * outer spec (so its EX_CLOSURE construction stores the float-correct thunk
     * + env) and intern a matching inner-body clone below.  Only the float case
     * needs this: every int64-register kind round-trips through the carrier. */
    Binding *inner_closure = (!borrow_path && fd)
        ? fn_binding->returns_closure_fn_binding : NULL;
    /* Specialize the inner closure body for float when:
     *   - dispatch-free (original path), OR
     *   - all dispatches are typed (fn [..] R) bindings whose result_full_type
     *     carries a named TY_TYVAR -- Direction 3 in emit_expr.c derives the
     *     correct C dispatch type from the binding's resolved type.
     * Skip only when there are untyped dispatches (TY_PTR_VOID or TY_FN without
     * named-tyvar result) that Direction 3 cannot handle. */
    bool inner_float = inner_closure && !fn_binding->closure_return_dispatches_untyped &&
        emit_inner_closure_needs_float_spec(inner_closure, bindings, n_bindings);
    if (inner_float) abi_changes = true;
    if (!abi_changes && !instance_changes) {
        if (!borrow_path &&
            !emit_abi_call_is_generic_relay(ctx, call, items, n_items)) {
            emit_abi_note_carrier_call(ctx, fn_binding);
        }
        return;
    }

    uint32_t before_specs = ctx->n_abi_specializations;
    EmitAbiSpecialization *spec = emit_abi_intern_spec(
        ctx, fn_binding, fn_expr, fd, bindings, n_bindings,
        arg_types, n_spec_args, result_type, call);
    /* Interning the inner-closure spec below may realloc abi_specializations,
     * invalidating `spec`; refer to the outer spec by index afterward. */
    uint32_t outer_spec_idx = (uint32_t)(spec - ctx->abi_specializations);
    emit_abi_record_specialized_call(ctx, call, spec->clone_name);

    /* poly-closure-result-specialization (Stage B+C): intern a register-class-
     * correct clone of the lifted inner closure body and link it to this outer
     * spec.  The inner clone is emitted under its own bindings so its result /
     * params / env fields / dispatch typedefs all resolve through
     * emit_resolve_type to the concrete float; the outer spec's EX_CLOSURE
     * construction then stores the clone's thunk + uses its suffixed env. */
    if (inner_float && inner_closure) {
        const Expr *inner_expr = emit_abi_find_fn_expr(items, n_items, inner_closure);
        if (inner_expr && inner_expr->kind == EX_FN_DEF && inner_expr->as.fn_def_.fn) {
            FnDef *inner_fd = inner_expr->as.fn_def_.fn;
            Type inner_args[MAX_FN_ARITY];
            uint8_t inner_n = inner_fd->n_params;
            /* The inner closure binding's TY_FN includes the hidden env as arg 0,
             * so its arg_full_types are 1:1 with inner_fd->params.  Resolve each
             * to a *concrete* type via the spec bindings: the clone name and
             * forward decl use type_c_name(arg_types[i]) while the definition
             * uses emit_type_c_name (which resolves tyvars), so arg_types[i] MUST
             * be tyvar-free or the three disagree (decl/def signature mismatch). */
            for (uint8_t i = 0; i < inner_n; i++) {
                if (i == 0) {
                    /* param 0 is the env pointer -- concrete ptr<void> already */
                    inner_args[i] = inner_fd->param_types[0];
                    continue;
                }
                const Type *aft = (inner_closure->type.as.fn.arg_full_types &&
                                   i < inner_closure->type.as.fn.arity)
                    ? inner_closure->type.as.fn.arg_full_types[i] : NULL;
                inner_args[i] = emit_abi_instantiate_type(
                    aft ? aft : &inner_fd->param_types[i],
                    bindings, n_bindings, ctx->type_arena);
            }
            Type inner_res = inner_closure->type.as.fn.result_full_type
                ? emit_abi_instantiate_type(inner_closure->type.as.fn.result_full_type,
                                            bindings, n_bindings, ctx->type_arena)
                : emit_type_from_kind(inner_closure->type.as.fn.result_kind);
            EmitAbiSpecialization *inner_spec = emit_abi_intern_spec(
                ctx, inner_closure, inner_expr, inner_fd, bindings, n_bindings,
                inner_args, inner_n, inner_res, NULL);
            /* Build the suffixed env-struct symbol both sites agree on. */
            if (!inner_spec->env_name_override && inner_fd->closure &&
                inner_fd->closure->env_name) {
                Buf en; buf_init(&en);
                buf_puts(&en, inner_fd->closure->env_name->name);
                buf_puts(&en, "__spec__");
                append_sanitized_c_token(&en, type_c_name(inner_res));
                buf_putc(&en, '\0');
                inner_spec->env_name_override =
                    emit_arena_symbol(ctx->type_arena, en.data);
                buf_free(&en);
            }
            uint32_t inner_idx = (uint32_t)(inner_spec - ctx->abi_specializations);
            ctx->abi_specializations[outer_spec_idx].inner_closure_spec_idx = (int32_t)inner_idx;
        }
    }

    /* GHE2: a freshly-created spec whose body references generic fn-values must
     * clone those values per the same bindings.  Recurse into the spec body with
     * those bindings active so emit_abi_scan_expr's EX_CALL case can detect and
     * intern the child fn-value specializations (and nested ones).  Borrow specs
     * (fd==NULL) are emitted by the owning module and scanned there. */
    if (fd && fd->body && ctx->n_abi_specializations != before_specs) {
        uint32_t spec_idx = outer_spec_idx;
        /* current_abi_specialization may point into abi_specializations, which
         * the recursion can realloc; save/restore by index, not pointer. */
        const EmitAbiSpecialization *saved = ctx->current_abi_specialization;
        bool saved_in_table = saved >= ctx->abi_specializations &&
                              saved < ctx->abi_specializations + ctx->n_abi_specializations;
        uint32_t saved_idx = saved_in_table
            ? (uint32_t)(saved - ctx->abi_specializations) : 0;
        ctx->current_abi_specialization = &ctx->abi_specializations[spec_idx];
        emit_abi_scan_expr(ctx, fd->body, items, n_items);
        ctx->current_abi_specialization = saved_in_table
            ? &ctx->abi_specializations[saved_idx] : saved;
    }
}

/* GHE2: specialize a generic function referenced *as a value* (not called).
 *
 * The CGI fix (emit_call_name's emit_reresolve_method_call) re-resolves a
 * typeclass-method *call* inside a monomorphized constrained generic, but a
 * lifted comparator like (fn [a :K b :K] (eq? a b)) is passed by *address*:
 * its body bakes the carrier instance (Eq[int]) and is never re-emitted per K.
 * So a cstr-keyed map-assoc-g passes the int comparator and a distinct-pointer
 * lookup misses.
 *
 * This walks the argument expressions of `call` looking for a generic
 * fn-binding referenced as a value (EX_VAR / EX_FN_TO_FAT wrapping one) whose
 * type mentions a tyvar bound by `bindings`.  For each, it interns a child
 * specialization under those same bindings, then recurses into the child
 * clone's body so nested fn-values / method calls in turn specialize.  At emit
 * time, atom_var resolves the value reference to the child clone (see
 * emit_core.c).  The instance selection inside the child body is handled by the
 * already-landed CGI method-call re-resolution. */
static void emit_abi_scan_fn_values(EmitCtx *ctx, const Expr *call,
                                    const AbiTypeBinding *bindings, uint8_t n_bindings,
                                    const Expr **items, uint32_t n_items) {
    if (!call || call->kind != EX_CALL || !bindings || n_bindings == 0) return;
    for (uint32_t i = 0; i < call->as.call_.n_args; i++) {
        const Expr *arg = call->as.call_.args[i];
        while (arg && (arg->kind == EX_ASCRIBE || arg->kind == EX_FN_TO_FAT)) {
            arg = (arg->kind == EX_ASCRIBE) ? arg->as.ascribe_.inner
                                            : arg->as.fn_to_fat_.inner;
        }
        if (!arg || arg->kind != EX_VAR || arg->type.kind != TY_FN) continue;
        Binding *vb = arg->as.var.binding;
        if (!vb || vb->type.kind != TY_FN || !vb->is_global || vb->closure_fn_binding)
            continue;

        const Expr *vfn_expr = emit_abi_find_fn_expr(items, n_items, vb);
        if (!vfn_expr || !vfn_expr->as.fn_def_.fn) continue;
        FnDef *vfd = vfn_expr->as.fn_def_.fn;
        if (vfd->closure || !vfd->body) continue;
        if (vfd->body->kind == EX_INLINE_C &&
            !inline_c_has_ty_template(vfd->body->as.inline_c_.inline_c)) {
            continue;
        }

        /* Derive the value-fn's specialized arg/result types by instantiating
         * its full types through the outer bindings.  Only proceed when this
         * actually changes the ABI (i.e. the value-fn is generic in one of the
         * outer tyvars); otherwise the carrier clone is already correct. */
        bool abi_changes = false;
        Type v_args[MAX_FN_ARITY];
        uint8_t v_nargs = vfd->n_params;
        for (uint8_t a = 0; a < v_nargs; a++) {
            const Type *full = (vb->type.as.fn.arg_full_types &&
                                vb->type.as.fn.arg_full_types[a])
                ? vb->type.as.fn.arg_full_types[a]
                : &vfd->params[a]->type;
            Type generic_arg = full ? *full : vfd->param_types[a];
            v_args[a] = full
                ? emit_abi_instantiate_type(full, bindings, n_bindings, ctx->type_arena)
                : vfd->param_types[a];
            if (strcmp(type_c_name(generic_arg), type_c_name(v_args[a])) != 0)
                abi_changes = true;
        }
        Type v_generic_result = vb->type.as.fn.result_full_type
            ? *vb->type.as.fn.result_full_type
            : emit_type_from_kind(vb->type.as.fn.result_kind);
        Type v_result = emit_abi_instantiate_type(&v_generic_result, bindings,
                                                  n_bindings, ctx->type_arena);
        if (strcmp(type_c_name(v_generic_result), type_c_name(v_result)) != 0)
            abi_changes = true;
        if (!abi_changes) continue;

        uint32_t before = ctx->n_abi_specializations;
        EmitAbiSpecialization *child = emit_abi_intern_spec(
            ctx, vb, vfn_expr, vfd, bindings, n_bindings,
            v_args, v_nargs, v_result, NULL);
        /* Newly created: recurse into the clone body so nested fn-values
         * specialize too.  (Already-interned specs were scanned when created.) */
        if (ctx->n_abi_specializations != before) {
            (void)child;
            const EmitAbiSpecialization *saved = ctx->current_abi_specialization;
            ctx->current_abi_specialization = &ctx->abi_specializations[before];
            emit_abi_scan_expr(ctx, vfd->body, items, n_items);
            ctx->current_abi_specialization = saved;
        }
    }
}

static void emit_abi_scan_expr(EmitCtx *ctx, const Expr *e,
                               const Expr **items, uint32_t n_items) {
    if (!e) return;
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                emit_abi_scan_expr(ctx, e->as.program.items[i], items, n_items);
            }
            break;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn) {
                const Expr *saved_scan_fn = ctx->current_scan_fn;
                ctx->current_scan_fn = e;
                emit_abi_scan_expr(ctx, e->as.fn_def_.fn->body, items, n_items);
                ctx->current_scan_fn = saved_scan_fn;
            }
            break;
        case EX_DEF:
            emit_abi_scan_expr(ctx, e->as.def_.init, items, n_items);
            break;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                emit_abi_scan_expr(ctx, e->as.let_.bindings[i].init, items, n_items);
            }
            emit_abi_scan_expr(ctx, e->as.let_.body, items, n_items);
            break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                emit_abi_scan_expr(ctx, e->as.do_.items[i], items, n_items);
            }
            break;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                emit_abi_scan_expr(ctx, e->as.builtin.args[i], items, n_items);
            }
            break;
        case EX_IF:
            emit_abi_scan_expr(ctx, e->as.if_.cond, items, n_items);
            emit_abi_scan_expr(ctx, e->as.if_.then_, items, n_items);
            emit_abi_scan_expr(ctx, e->as.if_.else_or_null, items, n_items);
            break;
        case EX_WHILE:
            emit_abi_scan_expr(ctx, e->as.while_.cond, items, n_items);
            emit_abi_scan_expr(ctx, e->as.while_.body, items, n_items);
            break;
        case EX_CALL: {
            uint32_t before = ctx->n_specialized_calls;
            emit_abi_register_call(ctx, e, items, n_items, NULL);
            /* If register_call did not turn this into a specialization clone, it
             * is a direct carrier call; remember its target so the generic
             * definition is still emitted. */
            if (!e->as.call_.fn_expr && e->as.call_.fn_binding &&
                ctx->n_specialized_calls == before &&
                !emit_abi_call_is_generic_relay(ctx, e, items, n_items)) {
                emit_abi_note_carrier_call(ctx, e->as.call_.fn_binding);
            }
            /* GHE2: when scanning inside an active specialization, a call that
             * passes a generic fn as a *value* argument (e.g. map-assoc-eq with a
             * comparator) needs that value-fn cloned under the active bindings. */
            if (ctx->current_abi_specialization) {
                emit_abi_scan_fn_values(ctx, e,
                    ctx->current_abi_specialization->bindings,
                    ctx->current_abi_specialization->n_bindings,
                    items, n_items);
            }
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                emit_abi_scan_expr(ctx, e->as.call_.args[i], items, n_items);
            }
            emit_abi_scan_expr(ctx, e->as.call_.fn_expr, items, n_items);
            break;
        }
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++) {
                emit_abi_scan_expr(ctx, e->as.make_struct_.field_values[i], items, n_items);
            }
            break;
        case EX_GET_FIELD:
            emit_abi_scan_expr(ctx, e->as.get_field_.struct_expr, items, n_items);
            break;
        case EX_SET_FIELD:
            emit_abi_scan_expr(ctx, e->as.set_field_.receiver, items, n_items);
            emit_abi_scan_expr(ctx, e->as.set_field_.value, items, n_items);
            break;
        case EX_RETURN:
            emit_abi_scan_expr(ctx, e->as.return_.value, items, n_items);
            break;
        case EX_ASCRIBE:
            emit_abi_scan_expr(ctx, e->as.ascribe_.inner, items, n_items);
            break;
        case EX_CAST:
            emit_abi_scan_expr(ctx, e->as.cast_.expr, items, n_items);
            break;
        case EX_REINTERPRET:
            if (e->as.reinterpret_.expr && e->as.reinterpret_.expr->kind == EX_CALL) {
                const Expr *rc = e->as.reinterpret_.expr;
                uint32_t before = ctx->n_specialized_calls;
                emit_abi_register_call(ctx, rc, items, n_items, &e->type);
                if (!rc->as.call_.fn_expr && rc->as.call_.fn_binding &&
                    ctx->n_specialized_calls == before) {
                    emit_abi_note_carrier_call(ctx, rc->as.call_.fn_binding);
                }
                for (uint32_t i = 0; i < e->as.reinterpret_.expr->as.call_.n_args; i++) {
                    emit_abi_scan_expr(ctx, e->as.reinterpret_.expr->as.call_.args[i], items, n_items);
                }
                emit_abi_scan_expr(ctx, e->as.reinterpret_.expr->as.call_.fn_expr, items, n_items);
            } else {
                emit_abi_scan_expr(ctx, e->as.reinterpret_.expr, items, n_items);
            }
            break;
        default:
            break;
    }
}

/* Phase I: --emit-abi-trace -- classify each resolved call site by the C-level
 * ABI path it takes, then print one line per call to stderr.  Runs after the
 * abi scan pass has populated ctx->specialized_call_exprs / abi_specializations
 * so the classification matches what emit actually emits. */
typedef enum {
    ABI_PATH_CONCRETE_CLONE,
    ABI_PATH_DICTIONARY,
    ABI_PATH_POLY_WRAPPER,
    ABI_PATH_CARRIER,
} AbiTracePath;

static const char *abi_trace_path_name(AbiTracePath p) {
    switch (p) {
        case ABI_PATH_CONCRETE_CLONE: return "concrete-clone";
        case ABI_PATH_DICTIONARY:     return "dictionary";
        case ABI_PATH_POLY_WRAPPER:   return "polymorphic-wrapper";
        case ABI_PATH_CARRIER:        return "carrier";
    }
    return "carrier";
}

/* Mirror emit_call_name's specialization lookup: an exact call-expr match, then
 * a binding + arg-type match against the specialization table.  Returns the
 * clone name (borrowed) when the call resolves to a concrete clone, else NULL. */
static const char *abi_trace_clone_name(const EmitCtx *ctx, const Expr *call) {
    if (!ctx || !call) return NULL;
    for (uint32_t i = 0; i < ctx->n_specialized_calls; i++) {
        if (ctx->specialized_call_exprs[i] == call) {
            return ctx->specialized_call_names[i];
        }
    }
    const Binding *b = call->kind == EX_CALL ? call->as.call_.fn_binding : NULL;
    if (call->kind == EX_CALL && b) {
        for (uint32_t si = 0; si < ctx->n_abi_specializations; si++) {
            const EmitAbiSpecialization *spec = &ctx->abi_specializations[si];
            if (spec->binding != b || spec->n_args != call->as.call_.n_args) continue;
            bool args_match = true;
            for (uint32_t ai = 0; ai < call->as.call_.n_args; ai++) {
                const Expr *cur = call->as.call_.args[ai];
                while (cur && cur->kind == EX_ASCRIBE) cur = cur->as.ascribe_.inner;
                Type actual = (cur && cur->kind == EX_REINTERPRET && cur->as.reinterpret_.expr)
                    ? cur->as.reinterpret_.expr->type
                    : (cur ? cur->type : emit_type_from_kind(TY_UNKNOWN));
                if (!type_eq(spec->arg_types[ai], actual)) { args_match = false; break; }
            }
            if (args_match) return spec->clone_name;
        }
    }
    return NULL;
}

static void emit_abi_trace_call(const EmitCtx *ctx, const Expr *call) {
    const char *callee = "<indirect>";
    if (call->as.call_.fn_binding && call->as.call_.fn_binding->name) {
        callee = call->as.call_.fn_binding->name->name;
    }
    const char *clone = abi_trace_clone_name(ctx, call);
    AbiTracePath path;
    if (clone) {
        path = ABI_PATH_CONCRETE_CLONE;
    } else if (call->as.call_.is_poly_call) {
        path = ABI_PATH_POLY_WRAPPER;
    } else if (call->as.call_.dict_arg) {
        path = ABI_PATH_DICTIONARY;
    } else {
        path = ABI_PATH_CARRIER;
    }
    fprintf(stderr, "abi-trace %u:%u %s %s",
            call->span.line, call->span.col_start, callee, abi_trace_path_name(path));
    if (clone) fprintf(stderr, " %s", clone);
    fputc('\n', stderr);
}

static void emit_abi_trace_expr(const EmitCtx *ctx, const Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                emit_abi_trace_expr(ctx, e->as.program.items[i]);
            break;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn) emit_abi_trace_expr(ctx, e->as.fn_def_.fn->body);
            break;
        case EX_DEF:
            emit_abi_trace_expr(ctx, e->as.def_.init);
            break;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                emit_abi_trace_expr(ctx, e->as.let_.bindings[i].init);
            emit_abi_trace_expr(ctx, e->as.let_.body);
            break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                emit_abi_trace_expr(ctx, e->as.do_.items[i]);
            break;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                emit_abi_trace_expr(ctx, e->as.builtin.args[i]);
            break;
        case EX_IF:
            emit_abi_trace_expr(ctx, e->as.if_.cond);
            emit_abi_trace_expr(ctx, e->as.if_.then_);
            emit_abi_trace_expr(ctx, e->as.if_.else_or_null);
            break;
        case EX_WHILE:
            emit_abi_trace_expr(ctx, e->as.while_.cond);
            emit_abi_trace_expr(ctx, e->as.while_.body);
            break;
        case EX_CALL:
            emit_abi_trace_call(ctx, e);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                emit_abi_trace_expr(ctx, e->as.call_.args[i]);
            emit_abi_trace_expr(ctx, e->as.call_.fn_expr);
            break;
        case EX_MATCH:
            emit_abi_trace_expr(ctx, e->as.match_.scrutinee);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                emit_abi_trace_expr(ctx, e->as.match_.arms[i].body);
                emit_abi_trace_expr(ctx, e->as.match_.arms[i].guard);
            }
            break;
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                emit_abi_trace_expr(ctx, e->as.make_struct_.field_values[i]);
            break;
        case EX_GET_FIELD:
            emit_abi_trace_expr(ctx, e->as.get_field_.struct_expr);
            break;
        case EX_SET_FIELD:
            emit_abi_trace_expr(ctx, e->as.set_field_.receiver);
            emit_abi_trace_expr(ctx, e->as.set_field_.value);
            break;
        case EX_SET:
            emit_abi_trace_expr(ctx, e->as.set_.value);
            break;
        case EX_RETURN:
            emit_abi_trace_expr(ctx, e->as.return_.value);
            break;
        case EX_ASCRIBE:
            emit_abi_trace_expr(ctx, e->as.ascribe_.inner);
            break;
        case EX_CAST:
            emit_abi_trace_expr(ctx, e->as.cast_.expr);
            break;
        case EX_REINTERPRET:
            emit_abi_trace_expr(ctx, e->as.reinterpret_.expr);
            break;
        case EX_POLY_WRAP:
            emit_abi_trace_expr(ctx, e->as.poly_wrap_.inner);
            break;
        default:
            break;
    }
}

/* TS4P1: Walk all expressions and register concrete ADT-app types for monomorphisation. */
static void scan_adt_apps_in_expr(const Expr *e) {
    if (!e) return;
    /* If this expression's type is a TY_APP, try to register it as a concrete ADT app. */
    if (e->type.kind == TY_APP) {
        (void)type_register_adt_app(e->type);
    }
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                scan_adt_apps_in_expr(e->as.program.items[i]);
            break;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn) scan_adt_apps_in_expr(e->as.fn_def_.fn->body);
            break;
        case EX_DEF:
            scan_adt_apps_in_expr(e->as.def_.init);
            break;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                scan_adt_apps_in_expr(e->as.let_.bindings[i].init);
            scan_adt_apps_in_expr(e->as.let_.body);
            break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                scan_adt_apps_in_expr(e->as.do_.items[i]);
            break;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                scan_adt_apps_in_expr(e->as.builtin.args[i]);
            break;
        case EX_IF:
            scan_adt_apps_in_expr(e->as.if_.cond);
            scan_adt_apps_in_expr(e->as.if_.then_);
            scan_adt_apps_in_expr(e->as.if_.else_or_null);
            break;
        case EX_WHILE:
            scan_adt_apps_in_expr(e->as.while_.cond);
            scan_adt_apps_in_expr(e->as.while_.body);
            break;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                scan_adt_apps_in_expr(e->as.call_.args[i]);
            scan_adt_apps_in_expr(e->as.call_.fn_expr);
            break;
        case EX_MATCH:
            scan_adt_apps_in_expr(e->as.match_.scrutinee);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                scan_adt_apps_in_expr(e->as.match_.arms[i].body);
                if (e->as.match_.arms[i].guard)
                    scan_adt_apps_in_expr(e->as.match_.arms[i].guard);
            }
            break;
        case EX_RETURN:
            scan_adt_apps_in_expr(e->as.return_.value);
            break;
        case EX_ASCRIBE:
            scan_adt_apps_in_expr(e->as.ascribe_.inner);
            break;
        case EX_CAST:
            scan_adt_apps_in_expr(e->as.cast_.expr);
            break;
        case EX_REINTERPRET:
            scan_adt_apps_in_expr(e->as.reinterpret_.expr);
            break;
        default:
            break;
    }
}

static bool emit_abi_fn_is_generic_unsafe(const Expr *e) {
    if (!e || e->kind != EX_FN_DEF || e->type.kind != TY_FN) return false;
    if (e->type.as.fn.result_full_type &&
        e->type.as.fn.result_full_type->kind != TY_TYVAR &&
        emit_abi_type_has_named_tyvar(e->type.as.fn.result_full_type)) {
        return true;
    }
    if (e->type.as.fn.arg_full_types) {
        for (uint8_t i = 0; i < e->type.as.fn.arity; i++) {
            const Type *arg = e->type.as.fn.arg_full_types[i];
            if (arg && arg->kind != TY_TYVAR && emit_abi_type_has_named_tyvar(arg)) {
                return true;
            }
        }
    }
    return false;
}

static bool emit_abi_has_carrier_call(const EmitCtx *ctx, const Binding *binding) {
    if (!ctx || !binding) return false;
    for (uint32_t i = 0; i < ctx->n_carrier_call_bindings; i++) {
        if (ctx->carrier_call_bindings[i] == binding) return true;
    }
    return false;
}

/* Decide whether emit_fn_def should be skipped for a top-level generic
 * function.  A generic-unsafe function (one with a named type variable nested
 * inside a compound arg/result type) is normally emitted only as per-callsite
 * specialization clones, so its non-specialized "carrier" definition is
 * suppressed to avoid an unused/ill-typed duplicate.
 *
 * KB-022: that suppression is only sound when callsites actually produced
 * specializations.  A fully-generic function such as `equal-cong`, whose
 * result type permanently mentions an unbound kind variable (`(Equal (f a)
 * (f b))`), can never be monomorphized -- every type in its signature lowers
 * to the int64_t carrier -- and no clone is ever generated.  Skipping it then
 * leaves a dangling call.  Emit the carrier definition whenever a direct
 * (non-specialized) call to the binding was observed during the abi scan;
 * otherwise keep suppressing it (the function is either unused here or fully
 * served by specialization clones, and its body may not be carrier-safe). */
static bool emit_abi_fn_skip_generic(const EmitCtx *ctx, const Expr *e) {
    if (!emit_abi_fn_is_generic_unsafe(e)) return false;
    if (e->kind != EX_FN_DEF || !e->as.fn_def_.fn) return false;
    return !emit_abi_has_carrier_call(ctx, e->as.fn_def_.fn->binding);
}

/* J1: Scan all items for ABI-specialization opportunities. */
static void emit_abi_scan_program(EmitCtx *ctx, const Expr **items, uint32_t n_items) {
    for (uint32_t i = 0; i < n_items; i++) {
        emit_abi_scan_expr(ctx, items[i], items, n_items);
    }
}

/* J3: Forward-declare a specialization clone.
 * Whole-program mode (external_linkage=false): emits 'static <ret> <clone>(...);'
 * Separate-compilation mode (external_linkage=true): omits 'static' so the
 * definition in the owning .c gets external linkage.
 * Borrow specs (fn==NULL) are skipped; they get their decl from the owner's header. */
static void emit_abi_forward_decl(Buf *out, const EmitAbiSpecialization *spec) {
    if (!spec || !spec->fn) return;
    if (!spec->external_linkage) buf_puts(out, "static ");
    buf_puts(out, type_c_name(spec->result_type));
    buf_printf(out, " %s(", spec->clone_name);
    for (uint8_t i = 0; i < spec->n_args; i++) {
        if (i > 0) buf_puts(out, ", ");
        if (spec->fn->params[i]->is_poly_fn) {
            buf_puts(out, "tur_poly_fn_t");
        } else if (spec->fn->param_types[i].kind == TY_FN) {
            buf_puts(out, "int64_t");
        } else {
            buf_puts(out, type_c_name(spec->arg_types[i]));
        }
    }
    buf_puts(out, ");\n");
}

/* Emit C forward declarations for every EX_FN_DEF in items.  Used by both
 * emit_program (single-file) and emit_implementation (separate compilation)
 * so that mutually-recursive static functions resolve at C-compile time. */
static void emit_fn_forward_decls(EmitCtx *ctx, Buf *out,
                                  const Expr **items, uint32_t n_items) {
    for (uint32_t i = 0; i < n_items; i++) {
        const Expr *e = items[i];
        if (e->kind != EX_FN_DEF) continue;
        FnDef *fd = e->as.fn_def_.fn;
        if (strcmp(fd->binding->name->name, "main") == 0) continue;
        if (emit_abi_fn_skip_generic(ctx, e)) continue;
        if (!ctx->separate_compilation || !fd->binding->is_exported) {
            buf_puts(out, "static ");
        }
        if (e->type.kind == TY_FN) {
            TypeKind result = e->type.as.fn.result_kind;
            /* RT/SC5: carrier-return bridge -- must mirror the definition path
             * in emit_fns.c so the forward declaration agrees with the body. */
            Type carrier_override = emit_carrier_return_override(fd);
            if (carrier_override.kind == TY_STRUCT) {
                buf_puts(out, type_c_name(carrier_override));
            } else if (e->type.as.fn.result_full_type &&
                       emit_inst_fn_return_carrier(fd, e->type.as.fn.result_full_type)) {
                /* instance-method-closure-return: mirror emit_fns.c so the
                 * forward declaration agrees with the definition and the dict
                 * field -- thin fn-ptr typedef or int64_t closure carrier. */
                buf_puts(out, emit_inst_fn_return_carrier(fd, e->type.as.fn.result_full_type));
            } else if (e->type.as.fn.result_full_type) {
                bool body_is_inline_c = (fd->body && fd->body->kind == EX_INLINE_C);
                const struct Type *rft = e->type.as.fn.result_full_type;
                /* fn-typed-return: mirror emit_fns.c -- a declared function-value
                 * return lowers to its matching fn-ptr typedef (skipped for ^fat
                 * returns, which become a void* heap fat box). */
                const char *fn_ret_td = e->type.as.fn.result_fat
                    ? NULL : emit_fn_return_typedef(fd, rft);
                /* ptr-generic-parameterised-type: a typed ptr<T> return lowers
                 * to `T *` even for inline-C bodies; mirror emit_fns.c. */
                bool typed_ptr = rft && rft->kind == TY_PTR_VOID && rft->as.ptr.inner;
                /* inline-c-struct-return: mirror emit_fns.c -- a by-value struct
                 * return lowers to the struct's C name even for inline-C bodies. */
                bool typed_struct = rft && rft->kind == TY_STRUCT;
                if (fn_ret_td && !body_is_inline_c) {
                    buf_puts(out, fn_ret_td);
                } else if (rft && (!body_is_inline_c || typed_ptr || typed_struct)) {
                    buf_puts(out, type_c_name(*rft));
                } else {
                    buf_puts(out, "int64_t");
                }
            } else {
                /* SF-application carrier bridge (forward decl mirror): when the
                 * outer fn type lost its result_full_type but the body's type
                 * has the concrete struct/app layout, use the body's type so
                 * the forward decl agrees with the body emission. */
                const char *_body_c = (fd->body && (fd->body->type.kind == TY_APP
                                                    || fd->body->type.kind == TY_STRUCT))
                    ? type_c_name(fd->body->type) : NULL;
                if (_body_c && strcmp(_body_c, "int64_t") != 0) {
                    buf_puts(out, _body_c);
                } else {
                    buf_puts(out, type_c_name(emit_type_from_kind(result)));
                }
            }
        } else {
            buf_puts(out, "void");
        }
        const char *fn_name = raw_name_for_binding(fd->binding);
        buf_printf(out, " %s(", fn_name);
        for (uint8_t j = 0; j < fd->n_params; j++) {
            if (j > 0) buf_puts(out, ", ");
            if (fd->params[j]->is_poly_fn) {
                buf_puts(out, "tur_poly_fn_t");
            } else if (fd->param_types[j].kind == TY_FN) {
                buf_puts(out, "int64_t");
            } else if (fd->params[j]->is_fat &&
                       fd->body && fd->body->kind == EX_INLINE_C) {
                /* fat-param-emitted-as-void-ptr-warns-in-inline-c.md: ^fat
                 * carrier handle -> int64_t for an inline-C body (matches the
                 * definition signature). */
                buf_puts(out, "int64_t");
            } else {
                bool _fwd_inline_c = (fd->body && fd->body->kind == EX_INLINE_C);
                /* bare-fat-sink-poly-box-slot0-int64-mismatch.md: a ^fat param's
                 * arg_full_types may hold a synthesized fn signature for the box
                 * site; emit the carrier from param_types so it does not leak into
                 * the forward declaration's param type. */
                Type _fwd_pty = (!fd->params[j]->is_fat &&
                                 e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[j])
                    ? *e->type.as.fn.arg_full_types[j]
                    : fd->param_types[j];
                if (!fd->closure && !_fwd_inline_c && type_struct_pass_by_ptr(_fwd_pty)) {
                    buf_printf(out, "const %s *", type_c_name(_fwd_pty));
                } else {
                    buf_puts(out, type_c_name(_fwd_pty));
                }
            }
        }
        buf_puts(out, ");\n");
        free((void*)fn_name);
    }
    /* CPS3: forward declarations for __cps wrappers */
    if (g_cps_path) {
        for (uint32_t i = 0; i < n_items; i++) {
            const Expr *e = items[i];
            if (e->kind != EX_FN_DEF) continue;
            FnDef *fd = e->as.fn_def_.fn;
            if (!fd->is_cps) continue;
            if (fd->closure) continue;
            if (strcmp(fd->binding->name->name, "main") == 0) continue;
            const char *fn_name = raw_name_for_binding(fd->binding);
            buf_printf(out, "static void %s__cps(tur_cps_cont_t *__k", fn_name);
            for (uint8_t j = 0; j < fd->n_params; j++) {
                buf_puts(out, ", ");
                if (fd->params[j]->is_poly_fn) {
                    buf_puts(out, "tur_poly_fn_t");
                } else if (fd->param_types[j].kind == TY_FN) {
                    buf_puts(out, "int64_t");
                } else if (fd->params[j]->is_fat &&
                           fd->body && fd->body->kind == EX_INLINE_C) {
                    /* fat-param-emitted-as-void-ptr-warns-in-inline-c.md: ^fat
                     * carrier handle -> int64_t for an inline-C body (matches the
                     * wrapper signature; inline-C is never CPS in practice). */
                    buf_puts(out, "int64_t");
                } else {
                    Type _pty = (!fd->params[j]->is_fat &&
                                 e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[j])
                        ? *e->type.as.fn.arg_full_types[j]
                        : fd->param_types[j];
                    buf_puts(out, type_c_name(_pty));
                }
                const char *pn = raw_name_for_binding(fd->params[j]);
                buf_printf(out, " %s", pn);
                free((void*)pn);
            }
            buf_puts(out, ");\n");
            free((void*)fn_name);
        }
    }
}

/* prelude-macros (Defect B / F3): emit the user-callable runtime `cons`
 * cons-cell builder into a translation unit's preamble when g_uses_cons is set.
 * Wrapped in an include guard so a TU that pulls the definition in through more
 * than one path never sees a duplicate `static` definition.  The cell layout
 * ({head,tail} int64) matches __tur_cons_of / tcons, so cons cells interoperate
 * with the head/tail walkers in stdlib/list.tur.  Registering `cons` this way
 * (rather than as a stdlib defn) makes it resolve in both single-file and
 * project/separate-compilation mode without the stdlib auto-load project mode
 * skips. */
static void emit_cons_helper(Buf *out) {
    if (!g_uses_cons) return;
    buf_puts(out, "#ifndef __TUR_CONS_HELPER\n");
    buf_puts(out, "#define __TUR_CONS_HELPER\n");
    buf_puts(out, "/* F3: cons -- allocate a {head,tail} cons cell; return pointer as int64 */\n");
    buf_puts(out, "static int64_t cons(int64_t h, int64_t t) {\n");
    buf_puts(out, "    struct { int64_t head; int64_t tail; } *c = malloc(sizeof(*c));\n");
    buf_puts(out, "    c->head = h; c->tail = t;\n");
    buf_puts(out, "    return (int64_t)(intptr_t)c;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "#endif\n");
}

/* Pass 0 helper: emit the tagged-union `typedef struct tur_adt_<Name> { ... }`
 * plus one `ctor_<Ctor>` allocator per constructor for an ADT (defdata/defgadt).
 *
 * Shared by the whole-program path (emit_program) and the
 * separate-compilation implementation path (emit_implementation) so that an
 * ADT used only *inside* a module's .c -- e.g. one spliced in by a top-level
 * (load "stdlib/either.tur") -- gets its base layout typedef + constructors
 * regardless of build mode.  The header (emit_header) never emits the base ADT
 * typedef, only monomorphized type-applications, so without this the per-module
 * .c references `tur_adt_Either` / `ctor_Left` with no definition.  See
 * docs/reported/load-not-expanded-in-imported-or-project-modules.md. */
static void emit_adt_typedef_and_ctors(Buf *out, const AdtDef *def) {
    char adt_c_name[256];
    {
        char *_mn = mangle_field_name(def->name);
        snprintf(adt_c_name, sizeof(adt_c_name), "tur_adt_%s", _mn);
        free(_mn);
    }
    buf_printf(out, "typedef struct %s {\n", adt_c_name);
    buf_printf(out, "    int tag;\n");
    buf_printf(out, "    union {\n");
    for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
        CtorDef *ctor = def->ctors[ci];
        char *mctor = mangle_field_name(ctor->name);
        buf_printf(out, "        struct {");
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
            const char *ctype;
            switch (ctor->fields[fi].kind) {
                case TY_INT:      ctype = "int64_t"; break;
                case TY_BOOL:     ctype = "bool"; break;
                case TY_FLOAT:    ctype = "double"; break;
                case TY_CSTR:     ctype = "const char *"; break;
                case TY_PTR_VOID: ctype = "void *"; break;
                case TY_RC:
                case TY_WEAK:     ctype = "RcControlBlock *"; break;
                case TY_REF:
                case TY_LREF:     ctype = "void *"; break;
                case TY_INT8:     ctype = "int8_t"; break;
                case TY_INT16:    ctype = "int16_t"; break;
                case TY_INT32:    ctype = "int32_t"; break;
                case TY_INT64:    ctype = "int64_t"; break;
                case TY_UINT8:    ctype = "uint8_t"; break;
                case TY_UINT16:   ctype = "uint16_t"; break;
                case TY_UINT32:   ctype = "uint32_t"; break;
                case TY_UINT64:   ctype = "uint64_t"; break;
                case TY_FLOAT32:  ctype = "float"; break;
                case TY_FLOAT64:  ctype = "double"; break;
                default:          ctype = "int64_t"; break;
            }
            buf_printf(out, " %s _%u;", ctype, fi);
        }
        buf_printf(out, " } %s;\n", mctor);
        free(mctor);
    }
    buf_printf(out, "    } as;\n");
    buf_printf(out, "} %s;\n\n", adt_c_name);

    /* Emit constructor functions */
    for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
        CtorDef *ctor = def->ctors[ci];
        char *mctor = mangle_field_name(ctor->name);
        buf_printf(out, "static int64_t ctor_%s(", mctor);
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
            if (fi > 0) buf_puts(out, ", ");
            const char *ctype;
            switch (ctor->fields[fi].kind) {
                case TY_INT:      ctype = "int64_t"; break;
                case TY_BOOL:     ctype = "bool"; break;
                case TY_FLOAT:    ctype = "double"; break;
                case TY_CSTR:     ctype = "const char *"; break;
                case TY_PTR_VOID: ctype = "void *"; break;
                case TY_RC:
                case TY_WEAK:     ctype = "RcControlBlock *"; break;
                case TY_REF:
                case TY_LREF:     ctype = "void *"; break;
                case TY_INT8:     ctype = "int8_t"; break;
                case TY_INT16:    ctype = "int16_t"; break;
                case TY_INT32:    ctype = "int32_t"; break;
                case TY_INT64:    ctype = "int64_t"; break;
                case TY_UINT8:    ctype = "uint8_t"; break;
                case TY_UINT16:   ctype = "uint16_t"; break;
                case TY_UINT32:   ctype = "uint32_t"; break;
                case TY_UINT64:   ctype = "uint64_t"; break;
                case TY_FLOAT32:  ctype = "float"; break;
                case TY_FLOAT64:  ctype = "double"; break;
                default:          ctype = "int64_t"; break;
            }
            buf_printf(out, "%s _%u", ctype, fi);
        }
        buf_printf(out, ") {\n");
        buf_printf(out, "    %s *__r = (%s *)malloc(sizeof(%s));\n",
                   adt_c_name, adt_c_name, adt_c_name);
        buf_printf(out, "    __r->tag = %u;\n", ctor->tag);
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
            buf_printf(out, "    __r->as.%s._%u = _%u;\n", mctor, fi, fi);
        }
        buf_printf(out, "    return (int64_t)(intptr_t)__r;\n");
        buf_printf(out, "}\n\n");
        free(mctor);
    }
}

int emit_program(Buf *out, const Expr *program) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "tur: emit: expected EX_PROGRAM\n");
        return -1;
    }

    /* Two buffers: file scope (statics) and main body. We assemble at the end. */
    Buf file; buf_init(&file);
    Buf body; buf_init(&body);
    /* Phase 19: separate buffers for ordered final assembly:
     *   early_file   - pass 0: struct typedefs + drop glue
     *   fwd_decls    - pass 1: function forward declarations
     *   extern_decls - user extern-c declarations
     *   file         - pass 2: function definitions + globals
     *   pending_handler_fns emitted between fwd_decls and file
     * Order ensures: struct typedefs visible to fwd_decls; fwd_decls visible
     * to handler functions; handler functions visible to fn definitions. */
    Buf early_file;  buf_init(&early_file);
    Buf thunk_typedefs; buf_init(&thunk_typedefs);
    Buf fwd_decls;   buf_init(&fwd_decls);
    Buf extern_decls; buf_init(&extern_decls);
    Buf defer_thunks; buf_init(&defer_thunks);
    /* file-scope-c-block: verbatim text of top-level ```c ... ``` blocks, emitted
     * at file scope (after typedefs/fwd-decls, before function definitions) so
     * they can define file-scope helper functions/structs that Turmeric defns
     * reference -- e.g. the capability vtables in stdlib/io.tur and log.tur. */
    Buf cprelude; buf_init(&cprelude);
    InlineCDedup cprelude_dedup = {0};  /* file-scope-inline-c-dedup */

    EmitCtx ctx;
    ctx.file = &file;
    ctx.main_ = &body;
    ctx.program_root = program;   /* cps-transform-plan (a): serial env instance scan */
    ctx.thunk_typedefs = &thunk_typedefs;
    ctx.indent = 4;
    ctx.tmp_n = 0;
    ctx.fn_params = NULL;
    ctx.n_fn_params = 0;
    /* Phase 3: closure tracking */
    ctx.closure = NULL;
    ctx.env_var_name = NULL;
    /* Phase 3: env struct tracking */
    ctx.env_struct_names = NULL;
    ctx.n_env_struct_names = 0;
    ctx.cap_env_struct_names = 0;
    ctx.thunk_typedef_names = NULL;
    ctx.n_thunk_typedef_names = 0;
    ctx.cap_thunk_typedef_names = 0;
    ctx.fatshim_names = NULL;
    ctx.n_fatshim_names = 0;
    ctx.cap_fatshim_names = 0;
    ctx.poly_fatshim_names = NULL;
    ctx.n_poly_fatshim_names = 0;
    ctx.cap_poly_fatshim_names = 0;
    /* Phase 4 v1: frame tracking */
    ctx.frame_var = NULL;
    ctx.in_scope_with_defers = false;
    ctx.pending_defer_thunks = NULL;
    /* Phase 4 v1: defer captures tracking */
    ctx.defer_captures = NULL;
    ctx.n_defer_captures = 0;
    /* Phase 3/4: Track return emission */
    ctx.return_emitted = false;
    /* Phase 19: Pending effect handler function buffer */
    Buf pending_hfns; buf_init(&pending_hfns);
    ctx.pending_handler_fns = &pending_hfns;
    /* Phase R5: no-unwind context (false at top level; set per-function) */
    ctx.no_unwind = false;
    /* Phase M3: emit_program always uses single-file (non-separate) mode */
    ctx.separate_compilation = false;
    /* Phase 19D: handle captures (NULL at top level) */
    ctx.handle_captures = NULL;
    ctx.n_handle_captures = 0;
    ctx.handle_env_name = NULL;
    /* GF1: generator struct context (NULL outside a _next function) */
    ctx.gen_struct_bindings = NULL;
    ctx.n_gen_struct_bindings = 0;
    ctx.gen_var_name = NULL;
    ctx.gen_struct_type = NULL;
    ctx.gen_hdr_emitted = false;
    ctx.abi_specializations = NULL;
    ctx.n_abi_specializations = 0;
    ctx.cap_abi_specializations = 0;
    ctx.specialized_call_exprs = NULL;
    ctx.specialized_call_names = NULL;
    ctx.n_specialized_calls = 0;
    ctx.cap_specialized_calls = 0;
    ctx.carrier_call_bindings = NULL;
    ctx.n_carrier_call_bindings = 0;
    ctx.cap_carrier_call_bindings = 0;
    ctx.current_abi_specialization = NULL;
    ctx.current_scan_fn = NULL;
    ctx.fn_name_override = NULL;
    ctx.fn_name_override_external = false;  /* J3: must match fn_name_override */
    ctx.n_pbp_params = 0;    /* Phase D: no pbp params at top level */
    /* ASan/LSan plan (Option C): arena for transient ABI-spec Type scratch,
     * freed in bulk at the end of this function. */
    Arena type_arena; arena_init(&type_arena, 0);
    ctx.type_arena = &type_arena;
    type_codegen_reset_struct_apps();
    type_codegen_reset_adt_apps();
    type_codegen_reset_fn_ptr_typedefs();
    sym_codegen_reset();   /* SYM1: clear interned-symbol records for this TU */

    /* Phase M0: Flatten program items, expanding EX_DEFMODULE body. */
    uint32_t n_items;
    const Expr **items = flatten_program_items(program, &n_items);

    /* J1: ABI specialization scan (extracted into emit_abi_scan_program). */
    emit_abi_scan_program(&ctx, items, n_items);

    /* Phase I: --emit-abi-trace -- report the resolved ABI path per call site. */
    if (g_emit_abi_trace) {
        for (uint32_t i = 0; i < n_items; i++) {
            emit_abi_trace_expr(&ctx, items[i]);
        }
    }

    /* TS4P1: Scan for concrete ADT-app types to register for monomorphisation. */
    for (uint32_t i = 0; i < n_items; i++) {
        scan_adt_apps_in_expr(items[i]);
    }

    /* Check if user defined a main function */
    bool user_has_main = false;
    for (uint32_t i = 0; i < n_items; i++) {
        const Expr *e = items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            if (strcmp(fd->binding->name->name, "main") == 0) {
                user_has_main = true;
                break;
            }
        }
    }

    /* Phase 2: Two-pass emission for mutual recursion support.
     * Pass 0: Emit struct typedefs + drop glue (must precede function forward decls). */
    for (uint32_t i = 0; i < n_items; i++) {
        const Expr *e = items[i];
        if (e->kind == EX_DEF && e->as.def_.struct_def) {
            StructDef *def = e->as.def_.struct_def;
            /* SI4-C: opaque types are just int64_t in C -- no typedef needed. */
            if (def->is_opaque) continue;
            /* Phase E: pre-register fn-ptr typedefs for concrete fn fields so
             * they are emitted before the struct typedef that references them. */
            for (uint32_t j = 0; j < def->n_fields; j++) {
                StructField *f = &def->fields[j];
                if (f->kind == TY_FN && f->full_type && f->full_type->kind == TY_FN) {
                    const char *td = register_fn_ptr_typedef(f->full_type);
                    if (td) {
                        type_codegen_emit_fn_ptr_typedefs(&early_file);
                    }
                }
            }
            /* Emit: typedef struct Name { fields... } Name; */
            buf_printf(&early_file, "typedef struct %s {\n", def->name);
            for (uint32_t j = 0; j < def->n_fields; j++) {
                StructField *f = &def->fields[j];
                const char *ctype;
                /* Phase E: typed function pointer for concrete fn fields. */
                if (f->kind == TY_FN && f->full_type && f->full_type->kind == TY_FN) {
                    const char *td = register_fn_ptr_typedef(f->full_type);
                    ctype = td ? td : "int64_t";
                } else switch (f->kind) {
                    case TY_INT:      ctype = "int64_t"; break;
                    case TY_BOOL:     ctype = "bool"; break;
                    case TY_FLOAT:    ctype = "double"; break;
                    case TY_CSTR:     ctype = "const char *"; break;
                    case TY_PTR_VOID: ctype = "void *"; break;
                    case TY_RC:
                    case TY_WEAK:     ctype = "RcControlBlock *"; break;
                    case TY_REF:
                    case TY_LREF:     ctype = "void *"; break;
                    /* Phase N6: new numeric field types */
                    case TY_INT8:     ctype = "int8_t"; break;
                    case TY_INT16:    ctype = "int16_t"; break;
                    case TY_INT32:    ctype = "int32_t"; break;
                    case TY_UINT8:    ctype = "uint8_t"; break;
                    case TY_UINT16:   ctype = "uint16_t"; break;
                    case TY_UINT32:   ctype = "uint32_t"; break;
                    case TY_UINT64:   ctype = "uint64_t"; break;
                    case TY_FLOAT32:  ctype = "float"; break;
                    default:          ctype = "int64_t"; break;
                }
                char *mfn = mangle_field_name(f->name);
                buf_printf(&early_file, "    %s %s;\n", ctype, mfn);
                free(mfn);
            }
            buf_printf(&early_file, "} %s;\n\n", def->name);
            /* If any RC/ref/weak field, emit drop glue function */
            if (def->needs_drop_glue) {
                buf_printf(&early_file, "static void drop_glue_%s(void *ptr) {\n", def->name);
                buf_printf(&early_file, "    if (!ptr) return;\n");
                buf_printf(&early_file, "    %s *s = (%s *)ptr;\n", def->name, def->name);
                /* Drop fields in REVERSE order */
                for (int32_t j = (int32_t)def->n_fields - 1; j >= 0; j--) {
                    StructField *f = &def->fields[j];
                    char *mfn = mangle_field_name(f->name);
                    if (f->kind == TY_RC) {
                        buf_printf(&early_file, "    if (s->%s) { rc_strong_decrement(s->%s); rc_free_queue_drain(); }\n",
                                   mfn, mfn);
                    } else if (f->kind == TY_WEAK) {
                        buf_printf(&early_file, "    if (s->%s) rc_weak_decrement(s->%s);\n",
                                   mfn, mfn);
                    } else if (f->kind == TY_REF || f->kind == TY_LREF) {
                        buf_printf(&early_file, "    if (s->%s) free(s->%s);\n",
                                   mfn, mfn);
                    }
                    free(mfn);
                }
                buf_printf(&early_file, "    free(ptr);\n");
                buf_printf(&early_file, "}\n\n");

                /* DS3: walk glue -- mirrors drop glue but calls a child
                 * callback for each rc-typed field instead of releasing.
                 * The cycle walker uses this to trace through struct
                 * payloads tagged RCK_STRUCT.  Weak / ref / lref fields
                 * are not strong owners and are not enumerated. */
                buf_printf(&early_file,
                           "static void walk_glue_%s(void *ptr, RcWalkChildFn cb, void *ctx) {\n",
                           def->name);
                buf_printf(&early_file, "    if (!ptr || !cb) return;\n");
                buf_printf(&early_file, "    %s *s = (%s *)ptr;\n", def->name, def->name);
                for (uint32_t j = 0; j < def->n_fields; j++) {
                    StructField *f = &def->fields[j];
                    if (f->kind == TY_RC) {
                        char *mfn = mangle_field_name(f->name);
                        buf_printf(&early_file, "    if (s->%s) cb(s->%s, ctx);\n",
                                   mfn, mfn);
                        free(mfn);
                    }
                }
                buf_printf(&early_file, "}\n\n");
            }
        }
        /* Phase G0/G1: ADT typedef + constructor functions */
        else if (e->kind == EX_DEFDATA || e->kind == EX_DEFGADT) {
            AdtDef *def = (e->kind == EX_DEFGADT) ? e->as.defgadt_.def : e->as.defdata_.def;
            emit_adt_typedef_and_ctors(&early_file, def);
        }
    }

    /* DV2+DV3: Emit TurDynFrame typedef, pthread_key_t globals, constructors,
     * cleanup functions, and (DV3) snapshot/convey infrastructure for every
     * defdynamic declaration found in the program. */
    {
        bool any_dynvar = false;
        for (uint32_t i = 0; i < n_items; i++) {
            if (items[i]->kind == EX_DEFDYNAMIC) { any_dynvar = true; break; }
        }
        if (any_dynvar) {
            /* DV2: TurDynFrame -- heap flag distinguishes DV3 snapshot frames
             * (heap=1, owned) from DV2 stack frames (heap=0, not owned). */
            buf_puts(&early_file,
                "/* DV2+DV3: dynamic var frame stack */\n"
                "typedef struct TurDynFrame {\n"
                "    struct TurDynFrame *prev;\n"
                "    void               *value;\n"
                "    int                 heap; /* DV3: 1 = frame+value are heap-allocated */\n"
                "} TurDynFrame;\n\n");

            /* DV2: per-var storage, destructor, constructor, and binding pop */
            for (uint32_t i = 0; i < n_items; i++) {
                const Expr *e = items[i];
                if (e->kind != EX_DEFDYNAMIC) continue;
                DynVarEntry *entry = e->as.defdynamic_.entry;
                char *mname = mangle_dynvar_name(entry->name->name);
                const char *ctype = type_c_name(entry->value_type);
                buf_printf(&early_file, "static %s _dynvar_root_%s;\n", ctype, mname);
                buf_printf(&early_file, "static pthread_key_t _dynvar_key_%s;\n", mname);
                /* DV3: destructor walks chain and frees heap frames on thread exit. */
                buf_printf(&early_file,
                    "static void _dynvar_cleanup_%s(void *f) {\n"
                    "    TurDynFrame *frame = (TurDynFrame *)f;\n"
                    "    while (frame && frame->heap) {\n"
                    "        TurDynFrame *prev = frame->prev;\n"
                    "        free(frame->value);\n"
                    "        free(frame);\n"
                    "        frame = prev;\n"
                    "    }\n"
                    "}\n"
                    "__attribute__((constructor))\n"
                    "static void _dynvar_init_%s(void) {\n"
                    "    pthread_key_create(&_dynvar_key_%s, _dynvar_cleanup_%s);\n"
                    "}\n"
                    "static void _dynvar_pop_%s(TurDynFrame **fp) {\n"
                    "    pthread_setspecific(_dynvar_key_%s, (*fp)->prev);\n"
                    "}\n\n",
                    mname,
                    mname, mname, mname,
                    mname, mname);
                free(mname);
            }

            /* DV3: _TurDynSnap struct -- one field pair per dynamic var. */
            buf_puts(&early_file, "/* DV3: binding snapshot for spawn-conveying */\n");
            buf_puts(&early_file, "typedef struct {\n");
            for (uint32_t i = 0; i < n_items; i++) {
                const Expr *e = items[i];
                if (e->kind != EX_DEFDYNAMIC) continue;
                DynVarEntry *entry = e->as.defdynamic_.entry;
                char *mname = mangle_dynvar_name(entry->name->name);
                const char *ctype = type_c_name(entry->value_type);
                buf_printf(&early_file,
                    "    int has_%s; %s val_%s;\n", mname, ctype, mname);
                free(mname);
            }
            buf_puts(&early_file, "} _TurDynSnap;\n\n");

            /* DV3: _tur_binding_snapshot_capture -- copy top frame value for each var. */
            buf_puts(&early_file,
                "static _TurDynSnap *_tur_binding_snapshot_capture(void) {\n"
                "    _TurDynSnap *s = (_TurDynSnap *)calloc(1, sizeof(_TurDynSnap));\n"
                "    if (!s) { fprintf(stderr, \"tur: oom\\n\"); abort(); }\n");
            for (uint32_t i = 0; i < n_items; i++) {
                const Expr *e = items[i];
                if (e->kind != EX_DEFDYNAMIC) continue;
                DynVarEntry *entry = e->as.defdynamic_.entry;
                char *mname = mangle_dynvar_name(entry->name->name);
                const char *ctype = type_c_name(entry->value_type);
                buf_printf(&early_file,
                    "    { TurDynFrame *_f = (TurDynFrame *)pthread_getspecific(_dynvar_key_%s);\n"
                    "      if (_f) { s->has_%s = 1; s->val_%s = *(%s *)_f->value; } }\n",
                    mname, mname, mname, ctype);
                free(mname);
            }
            buf_puts(&early_file, "    return s;\n}\n\n");

            /* DV3: _tur_binding_snapshot_install -- push heap frames on the new thread. */
            buf_puts(&early_file,
                "static void _tur_binding_snapshot_install(_TurDynSnap *s) {\n");
            for (uint32_t i = 0; i < n_items; i++) {
                const Expr *e = items[i];
                if (e->kind != EX_DEFDYNAMIC) continue;
                DynVarEntry *entry = e->as.defdynamic_.entry;
                char *mname = mangle_dynvar_name(entry->name->name);
                const char *ctype = type_c_name(entry->value_type);
                buf_printf(&early_file,
                    "    if (s->has_%s) {\n"
                    "        %s *_v = (%s *)malloc(sizeof(%s));\n"
                    "        if (!_v) { fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
                    "        *_v = s->val_%s;\n"
                    "        TurDynFrame *_fr = (TurDynFrame *)malloc(sizeof(TurDynFrame));\n"
                    "        if (!_fr) { free(_v); fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
                    "        _fr->prev  = (TurDynFrame *)pthread_getspecific(_dynvar_key_%s);\n"
                    "        _fr->value = _v;\n"
                    "        _fr->heap  = 1;\n"
                    "        pthread_setspecific(_dynvar_key_%s, _fr);\n"
                    "    }\n",
                    mname, ctype, ctype, ctype, mname, mname, mname);
                free(mname);
            }
            buf_puts(&early_file, "}\n\n");

            /* DV3: convey arg + trampoline + spawn-conveying entry point.
             * These reference TurThreadState/TurThreadHandle which are emitted
             * earlier in the output (before early_file is appended). */
            buf_puts(&early_file,
                "typedef struct {\n"
                "    int64_t         closure;\n"
                "    TurThreadState *state;\n"
                "    _TurDynSnap    *snap;\n"
                "} _TurConveyArg;\n\n"
                "static void *_tur_convey_trampoline(void *raw) {\n"
                "    _TurConveyArg *a = (_TurConveyArg *)raw;\n"
                "    tur_current_thread_state = a->state;\n"
                "    _tur_binding_snapshot_install(a->snap);\n"
                "    free(a->snap);\n"
                "    void (*fn)(void) = (void (*)(void))(intptr_t)a->closure;\n"
                "    free(a);\n"
                "    fn();\n"
                "    return NULL;\n"
                "}\n\n"
                "static void *_tur_spawn_conveying(int64_t closure) {\n"
                "    _TurDynSnap *snap = _tur_binding_snapshot_capture();\n"
                "    TurThreadState *state = (TurThreadState *)calloc(1, sizeof(TurThreadState));\n"
                "    if (!state) { free(snap); fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
                "    pthread_mutex_init(&state->cancel_mutex, NULL);\n"
                "    pthread_cond_init(&state->cancel_cond, NULL);\n"
                "    _TurConveyArg *arg = (_TurConveyArg *)malloc(sizeof(_TurConveyArg));\n"
                "    if (!arg) { free(state); free(snap); fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
                "    arg->closure = closure;\n"
                "    arg->state   = state;\n"
                "    arg->snap    = snap;\n"
                "    TurThreadHandle *h = (TurThreadHandle *)malloc(sizeof(TurThreadHandle));\n"
                "    if (!h) { free(arg); free(state); free(snap); fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
                "    h->state = state;\n"
                "    int _rc = pthread_create(&h->tid, NULL, _tur_convey_trampoline, arg);\n"
                "    if (_rc != 0) {\n"
                "        free(h); free(arg); free(state); free(snap);\n"
                "        fprintf(stderr, \"tur: spawn-conveying: pthread_create failed (%d)\\n\", _rc);\n"
                "        abort();\n"
                "    }\n"
                "    return (void *)h;\n"
                "}\n\n");
        }
    }

    /* Pass 1: Emit forward declarations for all functions.
     * Written to fwd_decls buffer (emitted before pending_handler_fns in final
     * assembly) so that effect handler functions can call user-defined functions. */
    emit_fn_forward_decls(&ctx, &fwd_decls, items, n_items);
    for (uint32_t i = 0; i < ctx.n_abi_specializations; i++) {
        emit_abi_forward_decl(&fwd_decls, &ctx.abi_specializations[i]);
    }

    /* Phase M5: collect top-level EX_DEFER nodes (module-level defers). */
    uint32_t n_prog_defers = 0;
    for (uint32_t i = 0; i < n_items; i++) {
        if (items[i]->kind == EX_DEFER) n_prog_defers++;
    }
    const Expr **prog_defers = NULL;
    if (n_prog_defers > 0) {
        prog_defers = (const Expr **)malloc(n_prog_defers * sizeof(Expr *));
        uint32_t di = 0;
        for (uint32_t i = 0; i < n_items; i++) {
            if (items[i]->kind == EX_DEFER)
                prog_defers[di++] = items[i];
        }
    }

    /* Pass 2: collect all top-level defs and fn_defs. */
    for (uint32_t i = 0; i < n_items; i++) {
        const Expr *e = items[i];
        if (e->kind == EX_DEFER) {
            /* Phase M5: module-level defers handled after this pass. */
            continue;
        } else if (e->kind == EX_DEFDATA) {
            /* Phase G0: ADT typedefs and constructor functions already emitted in Pass 0 */
            continue;
        } else if (e->kind == EX_DEF) {
            /* Phase 11: skip struct typedefs — already emitted in Pass 0 */
            if (e->as.def_.struct_def) continue;
            char *bn = name_for_binding(&ctx, e->as.def_.binding);
            buf_printf(&file, "static %s %s;\n",
                       type_c_name(e->as.def_.binding->type), bn);
            if (e->as.def_.init) {
                char *iv = emit_value(&ctx, &body, e->as.def_.init);
                indent_buf(&body, ctx.indent);
                buf_printf(&body, "%s = %s;\n", bn, iv);
                free(iv);
            }
            free(bn);
        } else if (e->kind == EX_FN_DEF) {
            /* Emit function definition at file scope */
            if (emit_abi_fn_skip_generic(&ctx, e)) {
                continue;
            }
            emit_fn_def(&ctx, &file, e);
        } else if (e->kind == EX_EXTERN_C) {
            /* Emit extern-c declaration early (before handler functions) */
            ExternC *ec = e->as.extern_c_.ext;
            /* When HAMT lowering is active, hamt.h is already included and
             * declares all tur_hamt_* functions; skip conflicting extern decls. */
            /* Also suppress redeclarations of C stdlib functions already in the preamble. */
            /* Functions declared in the runtime preamble (emit_runtime_preamble) or
             * via headers always included (<stdio.h>, <stdlib.h>).
             * Suppress redeclarations to avoid conflicting-types errors. */
            static const char *preamble_decls[] = {
                /* explicit extern decls in preamble */
                "malloc","calloc","free","abort","atexit",
                "memset","memmove","memcpy","memcmp","strcmp","strlen","strcpy","strncpy","strcat","strncat","strstr","strchr","strrchr","strdup",
                /* <stdio.h> */
                "printf","fprintf","sprintf","snprintf","scanf","sscanf","fscanf",
                "fopen","fclose","fread","fwrite","fseek","ftell","fflush","rewind",
                "puts","putchar","getchar","putc","getc","fputc","fgetc","fputs","fgets",
                "perror","clearerr","feof","ferror","remove","rename","tmpfile",
                /* <stdlib.h> */
                "exit","getenv","putenv","system","rand","srand","bsearch","qsort",
                "atoi","atol","atof","strtol","strtoul","strtod",
                NULL
            };
            bool suppress_ec = false;
            if (g_needs_hamt && strncmp(ec->c_name->name, "tur_hamt_", 9) == 0) {
                suppress_ec = true; /* Suppress: declared by #include "hamt.h" */
            } else {
                for (int si = 0; preamble_decls[si]; si++) {
                    if (strcmp(ec->c_name->name, preamble_decls[si]) == 0) {
                        suppress_ec = true; /* Suppress: already in preamble */
                        break;
                    }
                }
            }
            if (!suppress_ec) {
            /* extern-c names map to a real C symbol via the LEGACY fold (e.g.
             * `tur_hamt_new` stays itself; `tvar/new` -> `tvar_new`). This must
             * stay legacy -- never the injective scheme -- so the prototype, the
             * call sites (raw_name_for_binding special-cases is_extern_c), and
             * any inline-C reference all agree on the real symbol name. */
            char *ec_mangled = mangle_field_name(ec->c_name->name);
            buf_printf(&extern_decls, "extern %s %s(",
                       type_c_name(ec->return_type),
                       ec_mangled);
            free(ec_mangled);
            for (uint8_t j = 0; j < ec->n_params; j++) {
                if (j > 0) buf_puts(&extern_decls, ", ");
                buf_printf(&extern_decls, "%s", type_c_name(ec->param_types[j]));
            }
            buf_puts(&extern_decls, ");\n");
            }
        } else if (e->kind == EX_DEFDYNAMIC) {
            /* DV2: initialize the root value in main() body. */
            DynVarEntry *entry = e->as.defdynamic_.entry;
            char *mname = mangle_dynvar_name(entry->name->name);
            char *rv = emit_value(&ctx, &body, e->as.defdynamic_.root_expr);
            indent_buf(&body, ctx.indent);
            buf_printf(&body, "_dynvar_root_%s = %s;\n", mname, rv);
            free(rv);
            free(mname);
        } else if (e->kind == EX_INLINE_C) {
            /* file-scope-c-block: a top-level ```c ... ``` block is raw C emitted
             * verbatim at file scope, not a statement in main().  It carries no
             * captures/val-exprs, so emit its text directly. */
            InlineC *ic = e->as.inline_c_.inline_c;
            if (ic && ic->code.p && ic->code.len > 0 &&
                !inline_c_dedup_seen(&cprelude_dedup, ic->code.p, ic->code.len)) {
                buf_write(&cprelude, ic->code.p, ic->code.len);
                buf_putc(&cprelude, '\n');
            }
        } else {
            emit_stmt(&ctx, &body, e);
        }
    }
    for (uint32_t i = 0; i < ctx.n_abi_specializations; i++) {
        EmitAbiSpecialization *sp = &ctx.abi_specializations[i];
        /* poly-closure-result-specialization: hoist a linked inner-closure clone
         * ahead of its outer so the suffixed env struct is defined at file scope
         * before the outer body's EX_CLOSURE references it. */
        if (sp->inner_closure_spec_idx >= 0) {
            EmitAbiSpecialization *isp =
                &ctx.abi_specializations[sp->inner_closure_spec_idx];
            if (!isp->emitted) {
                ctx.current_abi_specialization = isp;
                ctx.fn_name_override = isp->clone_name;
                ctx.fn_name_override_external = false;
                emit_fn_def(&ctx, &file, isp->fn_expr);
                isp->emitted = true;
                ctx.fn_name_override = NULL;
                ctx.fn_name_override_external = false;
                ctx.current_abi_specialization = NULL;
                ctx.current_scan_fn = NULL;
            }
        }
        if (sp->emitted) continue;
        ctx.current_abi_specialization = sp;
        ctx.fn_name_override = sp->clone_name;
        ctx.fn_name_override_external = false;  /* single-file clones stay static */
        emit_fn_def(&ctx, &file, sp->fn_expr);
        sp->emitted = true;
        ctx.fn_name_override = NULL;
        ctx.fn_name_override_external = false;
        ctx.current_abi_specialization = NULL;
        ctx.current_scan_fn = NULL;
    }

    /* Phase M5: emit module-level defer thunks + atexit constructor. */
    if (n_prog_defers > 0) {
        buf_puts(&file, "\n/* Phase M5: module-level defers */\n");
        for (uint32_t i = 0; i < n_prog_defers; i++) {
            buf_printf(&file, "static void __module_defer_%u(void) {\n", i);
            Buf thunk_body; buf_init(&thunk_body);
            const char *saved_frame = ctx.frame_var;
            ctx.frame_var = NULL;
            ctx.indent = 4;
            emit_stmt(&ctx, &thunk_body, prog_defers[i]->as.defer_.body);
            ctx.frame_var = saved_frame;
            buf_write(&file, thunk_body.data, thunk_body.len);
            buf_free(&thunk_body);
            buf_puts(&file, "}\n");
        }
        buf_puts(&file,
            "__attribute__((constructor))\n"
            "static void __module_defers_init(void) {\n");
        for (uint32_t i = 0; i < n_prog_defers; i++) {
            buf_printf(&file, "    atexit(__module_defer_%u);\n", i);
        }
        buf_puts(&file, "}\n");
        free(prog_defers);
    }

    /* Final assembly. */
    buf_puts(out, "/* generated by tur (phase 2) */\n");
    /* Feature-test macro: must precede every #include so glibc exposes POSIX
     * declarations (clock_gettime, nanosleep, ...) used by the emitted runtime
     * even under a strict -std=c99 compile. No-op on Apple libc. */
    buf_puts(out, "#define _DEFAULT_SOURCE 1\n");
    /* Suppress warnings for unused helpers that are part of the runtime preamble
     * but not exercised by every program.  Both GCC and Clang honour these. */
    buf_puts(out, "#pragma GCC diagnostic ignored \"-Wunused-function\"\n");
    buf_puts(out, "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n");
    buf_puts(out, "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"\n");
    /* Phase P3: HAMT lowering - include HAMT header when needed */
    if (g_needs_hamt) {
        buf_puts(out, "#include \"hamt.h\"\n");
    }
    /* Phase T24: BSD networking headers (sys/socket.h, netinet/in.h, arpa/inet.h)
     * MUST come before ucontext.h.  On macOS, #define _XOPEN_SOURCE 700 suppresses
     * BSD extensions; once any header is processed with _XOPEN_SOURCE active, its
     * include guard prevents re-inclusion with BSD extensions enabled.  Including
     * networking headers first (without _XOPEN_SOURCE) lets them define INADDR_*,
     * sockaddr_in, etc., before ucontext.h locks in the POSIX-strict feature set. */
    buf_puts(out, "#include <sys/select.h>\n");
    buf_puts(out, "#include <sys/socket.h>\n");
    buf_puts(out, "#include <netinet/in.h>\n");
    buf_puts(out, "#include <arpa/inet.h>\n");
    /* Phase T21: ucontext.h must come before setjmp.h and pthread.h.
     * On macOS, setjmp.h indirectly includes a minimal ucontext.h without
     * _XOPEN_SOURCE, locking in the small 56-byte ucontext_t via include guards.
     * FiberBlock embeds two ucontext_t fields and needs the full 880-byte layout. */
    buf_puts(out, "#define _XOPEN_SOURCE 700\n");
    buf_puts(out, "#include <ucontext.h>\n");
    buf_puts(out, "#undef _XOPEN_SOURCE\n");
    buf_puts(out, "#include <setjmp.h>\n");
    buf_puts(out, "#include <stdio.h>\n");
    buf_puts(out, "#include <stdint.h>\n");
    buf_puts(out, "#include <stdbool.h>\n");
    /* Phase T19: Thread primitives - pthread on all supported platforms */
    buf_puts(out, "#include <pthread.h>\n");
    /* Phase 20-21: Software Transactional Memory */
    buf_puts(out, "#include <stdlib.h>\n");
    buf_puts(out, "#include <string.h>\n");
    /* POSIX regex (stdlib/re.tur): hoist regex.h to file scope so every
     * generated re_* function sees regex_t and friends. Per-function
     * `#include <regex.h>` only works for the first function due to header
     * include guards; lifting it here unblocks all re-module functions.
     * Gated so non-regex programs don't churn codegen snapshots. */
    if (g_needs_regex_h) {
        buf_puts(out, "#include <regex.h>\n");
    }
    /* AR8: Variadic rest-list cons-cell helper -- only emit when module has variadics */
    if (g_has_variadics) {
        buf_puts(out, "/* AR8: __tur_cons_of -- allocate and link a cons cell */\n");
        buf_puts(out, "typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;\n");
        buf_puts(out, "static int64_t __tur_cons_of(int64_t h, int64_t t) {\n");
        buf_puts(out, "    __tur_cons_cell *c = (__tur_cons_cell *)malloc(sizeof(__tur_cons_cell));\n");
        buf_puts(out, "    c->head = h; c->tail = t;\n");
        buf_puts(out, "    return (int64_t)(intptr_t)c;\n");
        buf_puts(out, "}\n");
    }
    /* prelude-macros (Defect B / F3): user-callable `cons` cons-cell builder. */
    emit_cons_helper(out);
    /* Phase X3: Set literal runtime — sorted-array representation (Option A, v1) */
    buf_puts(out, "/* Phase X3: tur_set_t — sorted int64_t array */\n");
    buf_puts(out, "typedef struct { int64_t *items; uint32_t n; } tur_set_t;\n");
    buf_puts(out, "static int __tur_set_cmp(const void *a, const void *b) {\n");
    buf_puts(out, "    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;\n");
    buf_puts(out, "    return (x > y) - (x < y);\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static tur_set_t *tur_set_from_items(uint32_t n, int64_t *src) {\n");
    buf_puts(out, "    tur_set_t *s = (tur_set_t *)malloc(sizeof(tur_set_t));\n");
    buf_puts(out, "    s->items = n ? (int64_t *)malloc(n * sizeof(int64_t)) : NULL;\n");
    buf_puts(out, "    if (n) memcpy(s->items, src, n * sizeof(int64_t));\n");
    buf_puts(out, "    if (n > 1) qsort(s->items, n, sizeof(int64_t), __tur_set_cmp);\n");
    buf_puts(out, "    uint32_t k = 0;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < n; i++)\n");
    buf_puts(out, "        if (k == 0 || s->items[k-1] != s->items[i]) s->items[k++] = s->items[i];\n");
    buf_puts(out, "    s->n = k;\n");
    buf_puts(out, "    return s;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static bool tur_set_member(tur_set_t *s, int64_t x) {\n");
    buf_puts(out, "    if (!s || !s->n) return false;\n");
    buf_puts(out, "    int lo = 0, hi = (int)s->n - 1;\n");
    buf_puts(out, "    while (lo <= hi) { int mid = (lo+hi)/2;\n");
    buf_puts(out, "        if (s->items[mid] == x) return true;\n");
    buf_puts(out, "        if (s->items[mid] < x) lo = mid+1; else hi = mid-1; }\n");
    buf_puts(out, "    return false;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static int64_t tur_set_count(tur_set_t *s) { return s ? (int64_t)s->n : 0; }\n");
    buf_puts(out, "static tur_set_t *tur_set_add(tur_set_t *s, int64_t x) {\n");
    buf_puts(out, "    if (tur_set_member(s, x)) {\n");
    buf_puts(out, "        tur_set_t *r = (tur_set_t *)malloc(sizeof(tur_set_t));\n");
    buf_puts(out, "        r->n = s->n; r->items = s->n ? (int64_t *)malloc(s->n*sizeof(int64_t)) : NULL;\n");
    buf_puts(out, "        if (s->n) memcpy(r->items, s->items, s->n*sizeof(int64_t));\n");
    buf_puts(out, "        return r;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tur_set_t *r = (tur_set_t *)malloc(sizeof(tur_set_t));\n");
    buf_puts(out, "    r->n = (s ? s->n : 0) + 1;\n");
    buf_puts(out, "    r->items = (int64_t *)malloc(r->n * sizeof(int64_t));\n");
    buf_puts(out, "    uint32_t pos = 0, base = s ? s->n : 0;\n");
    buf_puts(out, "    while (pos < base && s->items[pos] < x) pos++;\n");
    buf_puts(out, "    if (s && pos > 0) memcpy(r->items, s->items, pos*sizeof(int64_t));\n");
    buf_puts(out, "    r->items[pos] = x;\n");
    buf_puts(out, "    if (s && pos < base) memcpy(r->items+pos+1, s->items+pos, (base-pos)*sizeof(int64_t));\n");
    buf_puts(out, "    return r;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static tur_set_t *tur_set_remove(tur_set_t *s, int64_t x) {\n");
    buf_puts(out, "    if (!s || !s->n || !tur_set_member(s, x)) {\n");
    buf_puts(out, "        tur_set_t *r = (tur_set_t *)malloc(sizeof(tur_set_t));\n");
    buf_puts(out, "        r->n = s ? s->n : 0;\n");
    buf_puts(out, "        r->items = r->n ? (int64_t *)malloc(r->n*sizeof(int64_t)) : NULL;\n");
    buf_puts(out, "        if (r->n) memcpy(r->items, s->items, r->n*sizeof(int64_t));\n");
    buf_puts(out, "        return r;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tur_set_t *r = (tur_set_t *)malloc(sizeof(tur_set_t));\n");
    buf_puts(out, "    r->items = s->n > 1 ? (int64_t *)malloc((s->n-1)*sizeof(int64_t)) : NULL;\n");
    buf_puts(out, "    uint32_t k = 0;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < s->n; i++) if (s->items[i] != x) r->items[k++] = s->items[i];\n");
    buf_puts(out, "    r->n = k; return r;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static tur_set_t *tur_set_union(tur_set_t *a, tur_set_t *b) {\n");
    buf_puts(out, "    uint32_t na = a?a->n:0, nb = b?b->n:0, cap = na+nb;\n");
    buf_puts(out, "    int64_t *tmp = cap ? (int64_t *)malloc(cap*sizeof(int64_t)) : NULL;\n");
    buf_puts(out, "    if (a) memcpy(tmp, a->items, na*sizeof(int64_t));\n");
    buf_puts(out, "    if (b) memcpy(tmp+na, b->items, nb*sizeof(int64_t));\n");
    buf_puts(out, "    return tur_set_from_items(cap, tmp);\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static tur_set_t *tur_set_intersection(tur_set_t *a, tur_set_t *b) {\n");
    buf_puts(out, "    if (!a || !b || !a->n || !b->n) return tur_set_from_items(0, NULL);\n");
    buf_puts(out, "    int64_t *tmp = (int64_t *)malloc(a->n*sizeof(int64_t));\n");
    buf_puts(out, "    uint32_t k = 0;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < a->n; i++) if (tur_set_member(b, a->items[i])) tmp[k++] = a->items[i];\n");
    buf_puts(out, "    tur_set_t *r = tur_set_from_items(k, tmp); free(tmp); return r;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static tur_set_t *tur_set_difference(tur_set_t *a, tur_set_t *b) {\n");
    buf_puts(out, "    if (!a || !a->n) return tur_set_from_items(0, NULL);\n");
    buf_puts(out, "    int64_t *tmp = (int64_t *)malloc(a->n*sizeof(int64_t));\n");
    buf_puts(out, "    uint32_t k = 0;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < a->n; i++) if (!tur_set_member(b, a->items[i])) tmp[k++] = a->items[i];\n");
    buf_puts(out, "    tur_set_t *r = tur_set_from_items(k, tmp); free(tmp); return r;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void tur_set_free(tur_set_t *s) { if (s) { free(s->items); free(s); } }\n");
    /* Phase HRT1: rank-2 polymorphic function type.
     * tur_poly_fn_t is a generic closure: a function pointer paired with an env pointer.
     * Used for (forall [a] (-> a a))-style rank-2 parameters. */
    buf_puts(out, "/* Phase HRT1: rank-2 polymorphic function type */\n");
    buf_puts(out, "typedef struct { void *env; int64_t (*fn)(void *, int64_t); } tur_poly_fn_t;\n");
    /* ET3: handler runtime type.
     * tur_handler_t is a handler value: an env pointer plus a dispatch function.
     * The dispatch function receives: env, n_args, value, continuation-as-int64_t. */
    buf_puts(out, "/* ET3: algebraic effect handler runtime type */\n");
    buf_puts(out, "typedef struct { void *env; int64_t (*fn)(int64_t *, int, int64_t, void *); } tur_handler_t;\n");
    /* FH1: first-class handler value -- an effect-keyed dispatch table.
     * Generalizes tur_handler_t from one function to an array of per-effect
     * entries.  A single-effect handler literal yields a one-entry table;
     * compose-handlers concatenates two tables (FH5).  Each entry carries the
     * handled effect name (interned C-string literal), the generated case
     * function (same signature as tur_handler_t.fn), its captured-env pointer,
     * and the continuation discipline (cont_kind: matches CopyKind ordinals --
     * 0=unique/affine, 1=copy, 2=linear, 3=multishot).
     *
     * Lifetime (FH1.2): a handler value may outlive the scope that created it,
     * so both the entries array and each entry's env are heap-allocated.  The
     * table struct itself is heap-allocated and owns the entries array; each
     * env is owned by its entry.  tur_handler_table_free drops the whole table
     * (envs, array, then the struct).  Composition produces a fresh table that
     * borrows the source entries' fn/eff_name (static) but takes ownership of
     * copies of the env pointers via the table that created them; to keep
     * ownership unambiguous and ASan/LSan-clean, a composed table does not
     * double-free shared envs -- it is the application site (with-handler) that
     * frees, and only the outermost owner frees.  See
     * docs/first-class-handlers-semantics.md (FH1.2 invariant). */
    buf_puts(out, "/* FH1: first-class handler dispatch-table entry */\n");
    buf_puts(out, "typedef struct { const char *eff_name; int64_t (*fn)(int64_t *, int, int64_t, void *); void *env; uint8_t cont_kind; } tur_handler_entry_t;\n");
    buf_puts(out, "/* FH1: first-class handler value -- effect-keyed dispatch table */\n");
    buf_puts(out, "typedef struct { tur_handler_entry_t *entries; int n_entries; } tur_handler_table_t;\n");
    buf_puts(out, "static tur_handler_table_t *tur_handler_table_new(int n) {\n");
    buf_puts(out, "    tur_handler_table_t *t = (tur_handler_table_t *)calloc(1, sizeof(tur_handler_table_t));\n");
    buf_puts(out, "    t->entries = (tur_handler_entry_t *)calloc((size_t)(n > 0 ? n : 1), sizeof(tur_handler_entry_t));\n");
    buf_puts(out, "    t->n_entries = n; return t;\n");
    buf_puts(out, "}\n");
    /* FH5: concatenate two tables (h1's entries first; h1 is the outer handler
     * per FH0.1).  Consumes a and b: their entries (including env ownership) are
     * transferred into the new owning table, and their now-empty struct+array
     * shells are freed.  A composed table is therefore a single owning object
     * that tur_handler_table_free fully reclaims. */
    buf_puts(out, "static tur_handler_table_t *tur_handler_table_concat(tur_handler_table_t *a, tur_handler_table_t *b) {\n");
    buf_puts(out, "    int na = a ? a->n_entries : 0, nb = b ? b->n_entries : 0;\n");
    buf_puts(out, "    tur_handler_table_t *t = tur_handler_table_new(na + nb);\n");
    buf_puts(out, "    for (int i = 0; i < na; i++) t->entries[i] = a->entries[i];\n");
    buf_puts(out, "    for (int i = 0; i < nb; i++) t->entries[na + i] = b->entries[i];\n");
    buf_puts(out, "    if (a) { free(a->entries); free(a); }\n");
    buf_puts(out, "    if (b) { free(b->entries); free(b); }\n");
    buf_puts(out, "    return t;\n");
    buf_puts(out, "}\n");
    /* FH1.2: deep-free a handler value -- its entries' envs, the entries array,
     * then the struct.  Single owner frees; ASan/LSan-clean. */
    buf_puts(out, "static void tur_handler_table_free(tur_handler_table_t *t) {\n");
    buf_puts(out, "    if (!t) return;\n");
    buf_puts(out, "    for (int i = 0; i < t->n_entries; i++) free(t->entries[i].env);\n");
    buf_puts(out, "    free(t->entries); free(t);\n");
    buf_puts(out, "}\n");
    /* IT4: Tagged union runtime representation.
     * tur_tagged_t carries a discriminant tag and a 64-bit payload.
     * Used for (A | B) union types and the 'any' top type. */
    /* Phase C2: expose whether contracts are compiled in. When --no-contracts
     * is set the contract checks are already stripped at elaboration time;
     * this constant lets inline-C also branch on the build mode. */
    buf_printf(out, "#define TUR_CONTRACTS_ENABLED %d\n", g_no_contracts ? 0 : 1);

    buf_puts(out, "/* IT4: tagged union runtime representation */\n");
    buf_puts(out, "typedef struct { int64_t tag; int64_t val; } tur_tagged_t;\n");
    buf_puts(out, "#define TUR_TAG(t, v)   ((tur_tagged_t){(int64_t)(t), (int64_t)(v)})\n");
    buf_puts(out, "#define TUR_UNTAG(x)    ((x).val)\n");
    buf_puts(out, "#define TUR_GETTAG(x)   ((x).tag)\n");
    /* Pointer accessors for tur_tagged_t*.  Inline-C that allocates tagged
     * nodes on the heap should use these instead of raw ->tag / ->val so the
     * access site does not depend on the struct field layout. */
    buf_puts(out, "#define TUR_PTAG(p)     ((p)->tag)\n");
    buf_puts(out, "#define TUR_PVAL(p)     ((p)->val)\n");
    /* Fat-closure application helpers for inline-C blocks.
     * A fat closure is a heap struct { int64_t __fn; <captures...> }; the thunk
     * has signature (void *env, int64_t arg...) -> int64_t.  TUR_APPLYn reads the
     * thunk pointer from slot 0 and invokes it with the closure as its env, so
     * inline-C no longer hand-writes the ((int64_t(*)(void*, ...))...)[0] cast.
     * The closure handle f and the arguments are taken as int64_t (the polymorphic
     * carrier type used throughout the runtime). */
    buf_puts(out, "#define TUR_CLOSURE_FN(f)  ((int64_t *)(intptr_t)(f))[0]\n");
    /* Typed Closure Invocation ABI (closure-typed-invocation-abi-plan, Phase 1).
     * TUR_APPLYn_T speaks the closure's *declared* C types instead of forcing
     * everything through int64_t.  R is the closure's C return type; A0,A1,...
     * are its C argument types in order.  An inline-C block that knows the
     * concrete signature of a closure (e.g. a (fn [x :float] :float ...) signal
     * body, or a (fn [p :ptr<T>] :ptr<T> ...) iterator step) invokes it through
     * the matching _T form so the value never round-trips through an int64_t
     * bit-cast.  The thunk is still read from slot 0 and called with the closure
     * box as its env -- only the function-pointer cast changes.  Each argument
     * is coerced with (Ai)(value) at the call site. */
    buf_puts(out, "#define TUR_APPLY0_T(R, f) \\\n");
    buf_puts(out, "    (((R (*)(void *))(intptr_t)TUR_CLOSURE_FN(f)) \\\n");
    buf_puts(out, "        ((void *)(intptr_t)(f)))\n");
    buf_puts(out, "#define TUR_APPLY1_T(R, A0, f, a) \\\n");
    buf_puts(out, "    (((R (*)(void *, A0))(intptr_t)TUR_CLOSURE_FN(f)) \\\n");
    buf_puts(out, "        ((void *)(intptr_t)(f), (A0)(a)))\n");
    buf_puts(out, "#define TUR_APPLY2_T(R, A0, A1, f, a, b) \\\n");
    buf_puts(out, "    (((R (*)(void *, A0, A1))(intptr_t)TUR_CLOSURE_FN(f)) \\\n");
    buf_puts(out, "        ((void *)(intptr_t)(f), (A0)(a), (A1)(b)))\n");
    buf_puts(out, "#define TUR_APPLY3_T(R, A0, A1, A2, f, a, b, c) \\\n");
    buf_puts(out, "    (((R (*)(void *, A0, A1, A2))(intptr_t)TUR_CLOSURE_FN(f)) \\\n");
    buf_puts(out, "        ((void *)(intptr_t)(f), (A0)(a), (A1)(b), (A2)(c)))\n");
    buf_puts(out, "#define TUR_APPLY4_T(R, A0, A1, A2, A3, f, a, b, c, d) \\\n");
    buf_puts(out, "    (((R (*)(void *, A0, A1, A2, A3))(intptr_t)TUR_CLOSURE_FN(f)) \\\n");
    buf_puts(out, "        ((void *)(intptr_t)(f), (A0)(a), (A1)(b), (A2)(c), (A3)(d)))\n");
    /* Phase R2: nullary fat-closure apply -- a (fn [] :int) thunk is invoked
     * through the standard closure protocol (thunk = slot 0, env = the box).
     * The legacy TUR_APPLYn macros are the all-int64_t case of TUR_APPLYn_T,
     * kept as literal-equivalent shorthands so existing hand-written inline-C
     * (stdlib/arrow.tur et al.) compiles unchanged. */
    buf_puts(out, "#define TUR_APPLY0(f)          TUR_APPLY0_T(int64_t, f)\n");
    buf_puts(out, "#define TUR_APPLY1(f, a)       TUR_APPLY1_T(int64_t, int64_t, f, a)\n");
    buf_puts(out, "#define TUR_APPLY2(f, a, b)    TUR_APPLY2_T(int64_t, int64_t, int64_t, f, a, b)\n");
    buf_puts(out, "#define TUR_APPLY3(f, a, b, c) \\\n");
    buf_puts(out, "    TUR_APPLY3_T(int64_t, int64_t, int64_t, int64_t, f, a, b, c)\n");
    buf_puts(out, "#define TUR_APPLY4(f, a, b, c, d) \\\n");
    buf_puts(out, "    TUR_APPLY4_T(int64_t, int64_t, int64_t, int64_t, int64_t, f, a, b, c, d)\n");
    /* C#1 (test-suite-idioms): inline-C Option/Result ABI helpers.
     * A `:Option<int>` value is a heap pointer to { bool is_some; int64_t value; }
     * with none == NULL (0).  A `:Result<int,E>` value is a heap pointer to
     * { bool is_ok; int64_t ok_val; int64_t err_val; }.  These helpers let an
     * inline-C block construct and inspect Option/Result values through the
     * canonical layout instead of hand-rolling the struct cast + a magic
     * sentinel integer (`-1`, `INT64_MIN`, `0`-as-absent).  The layout matches
     * stdlib/option.tur and stdlib/result.tur byte-for-byte, so values built
     * with tur_some/tur_ok flow transparently into the stdlib accessors and
     * vice versa.  Marked unused so a program that touches neither type still
     * compiles clean under -Wall. */
    buf_puts(out, "typedef struct { bool is_some; int64_t value; } tur_option_t;\n");
    buf_puts(out, "typedef struct { bool is_ok; int64_t ok_val; int64_t err_val; } tur_result_box_t;\n");
    buf_puts(out, "#define TUR_NONE ((int64_t)0)\n");
    buf_puts(out, "static int64_t tur_some(int64_t __x) __attribute__((unused));\n");
    buf_puts(out, "static int64_t tur_some(int64_t __x) {\n");
    buf_puts(out, "    tur_option_t *__o = (tur_option_t *)malloc(sizeof(*__o));\n");
    buf_puts(out, "    __o->is_some = true; __o->value = __x;\n");
    buf_puts(out, "    return (int64_t)(intptr_t)__o;\n}\n");
    buf_puts(out, "static bool tur_is_some(int64_t __o) __attribute__((unused));\n");
    buf_puts(out, "static bool tur_is_some(int64_t __o) {\n");
    buf_puts(out, "    return __o != 0 && ((tur_option_t *)(intptr_t)__o)->is_some;\n}\n");
    buf_puts(out, "static int64_t tur_opt_value(int64_t __o) __attribute__((unused));\n");
    buf_puts(out, "static int64_t tur_opt_value(int64_t __o) {\n");
    buf_puts(out, "    return ((tur_option_t *)(intptr_t)__o)->value;\n}\n");
    buf_puts(out, "static int64_t tur_ok(int64_t __v) __attribute__((unused));\n");
    buf_puts(out, "static int64_t tur_ok(int64_t __v) {\n");
    buf_puts(out, "    tur_result_box_t *__r = (tur_result_box_t *)malloc(sizeof(*__r));\n");
    buf_puts(out, "    __r->is_ok = true; __r->ok_val = __v; __r->err_val = 0;\n");
    buf_puts(out, "    return (int64_t)(intptr_t)__r;\n}\n");
    buf_puts(out, "static int64_t tur_err(int64_t __e) __attribute__((unused));\n");
    buf_puts(out, "static int64_t tur_err(int64_t __e) {\n");
    buf_puts(out, "    tur_result_box_t *__r = (tur_result_box_t *)malloc(sizeof(*__r));\n");
    buf_puts(out, "    __r->is_ok = false; __r->ok_val = 0; __r->err_val = __e;\n");
    buf_puts(out, "    return (int64_t)(intptr_t)__r;\n}\n");
    buf_puts(out, "static bool tur_is_ok(int64_t __r) __attribute__((unused));\n");
    buf_puts(out, "static bool tur_is_ok(int64_t __r) {\n");
    buf_puts(out, "    return __r != 0 && ((tur_result_box_t *)(intptr_t)__r)->is_ok;\n}\n");
    buf_puts(out, "static int64_t tur_ok_value(int64_t __r) __attribute__((unused));\n");
    buf_puts(out, "static int64_t tur_ok_value(int64_t __r) {\n");
    buf_puts(out, "    return ((tur_result_box_t *)(intptr_t)__r)->ok_val;\n}\n");
    buf_puts(out, "static int64_t tur_err_value(int64_t __r) __attribute__((unused));\n");
    buf_puts(out, "static int64_t tur_err_value(int64_t __r) {\n");
    buf_puts(out, "    return ((tur_result_box_t *)(intptr_t)__r)->err_val;\n}\n");
    /* A#1: fat-closure auto-shim thunks.  EX_FN_TO_FAT allocates a 2-slot fat
     * struct { __fn = __tur_fatshim<arity>, __orig = bare_fn_ptr } so a non-capturing
     * fn passed to a ^fat parameter is invoked through the standard fat-closure
     * protocol (thunk = slot 0, env = the struct).  Each shim ignores the thunk
     * slot, reads the original fn pointer from slot 1, and forwards its arguments
     * using the int64_t carrier ABI (matching TUR_APPLY and the reactor casts).
     * This retires the historical capture-forcing dummy ((let [_ x] (fn ...))). */
    buf_puts(out, "static int64_t __tur_fatshim0(void *__e) {\n");
    buf_puts(out, "    return ((int64_t (*)(void))(intptr_t)((int64_t *)__e)[1])();\n}\n");
    buf_puts(out, "static int64_t __tur_fatshim1(void *__e, int64_t a0) {\n");
    buf_puts(out, "    return ((int64_t (*)(int64_t))(intptr_t)((int64_t *)__e)[1])(a0);\n}\n");
    buf_puts(out, "static int64_t __tur_fatshim2(void *__e, int64_t a0, int64_t a1) {\n");
    buf_puts(out, "    return ((int64_t (*)(int64_t, int64_t))(intptr_t)((int64_t *)__e)[1])(a0, a1);\n}\n");
    buf_puts(out, "static int64_t __tur_fatshim3(void *__e, int64_t a0, int64_t a1, int64_t a2) {\n");
    buf_puts(out, "    return ((int64_t (*)(int64_t, int64_t, int64_t))(intptr_t)((int64_t *)__e)[1])(a0, a1, a2);\n}\n");
    buf_puts(out, "static int64_t __tur_fatshim4(void *__e, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {\n");
    buf_puts(out, "    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))(intptr_t)((int64_t *)__e)[1])(a0, a1, a2, a3);\n}\n");
    buf_puts(out, "static int64_t __tur_fatshim5(void *__e, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {\n");
    buf_puts(out, "    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))(intptr_t)((int64_t *)__e)[1])(a0, a1, a2, a3, a4);\n}\n");
    /* SC7: EX_POLY_TO_FAT thunks.  Convert a tur_poly_fn_t {env,fn} (a
     * typeclass-method closure param) into the fat-closure protocol: the fat box
     * is { __tur_poly_to_fat<N>, fn, env }, and the sink's N-ary fat-call passes
     * the box as the env, so the thunk reads the original fn (slot 1) + its env
     * (slot 2) and forwards every argument.  The carrier stores the method's
     * real N-ary thunk in slot 1 (make_poly_wrapper builds it), so a binary or
     * higher-arity poly method round-trips when boxed into a ^fat sink of the
     * matching arity. */
    for (int n = 0; n <= 5; n++) {
        buf_printf(out, "static int64_t __tur_poly_to_fat%d(void *__e", n);
        for (int i = 0; i < n; i++) buf_printf(out, ", int64_t a%d", i);
        buf_puts(out, ") {\n    int64_t *__b = (int64_t *)__e;\n");
        buf_puts(out, "    return ((int64_t (*)(void *");
        for (int i = 0; i < n; i++) buf_puts(out, ", int64_t");
        buf_puts(out, "))(intptr_t)__b[1])((void *)(intptr_t)__b[2]");
        for (int i = 0; i < n; i++) buf_printf(out, ", a%d", i);
        buf_puts(out, ");\n}\n");
    }
    /* Suppress -Wunused-function for shim arities a program does not use. */
    buf_puts(out, "static void *__tur_fatshim_keep[] __attribute__((unused)) = {\n");
    buf_puts(out, "    (void *)__tur_fatshim0, (void *)__tur_fatshim1, (void *)__tur_fatshim2,\n");
    buf_puts(out, "    (void *)__tur_fatshim3, (void *)__tur_fatshim4, (void *)__tur_fatshim5,\n");
    buf_puts(out, "    (void *)__tur_poly_to_fat0, (void *)__tur_poly_to_fat1,\n");
    buf_puts(out, "    (void *)__tur_poly_to_fat2, (void *)__tur_poly_to_fat3,\n");
    buf_puts(out, "    (void *)__tur_poly_to_fat4, (void *)__tur_poly_to_fat5 };\n");
    /* IT4/TY2.4: (type-of x) helper — maps a TypeKind tag to a cstr type name.
     * The tag stored in tur_tagged_t is the value's TypeKind enum value, so the
     * struct/ADT cases are emitted from the actual enum constants rather than
     * hard-coded integers (their numeric values move as the enum grows).  Note
     * the tag carries kind granularity only: every struct shares the "struct"
     * tag and every ADT the "adt" tag (see the union-intersection guide). */
    buf_puts(out, "static const char *__tur_any_type_name(int64_t tag) {\n");
    buf_puts(out, "    switch (tag) {\n");
    buf_printf(out, "        case %d: return \"nil\";\n",   (int)TY_NIL);
    buf_printf(out, "        case %d: return \"bool\";\n",  (int)TY_BOOL);
    buf_printf(out, "        case %d: return \"int\";\n",   (int)TY_INT);
    buf_printf(out, "        case %d: return \"float\";\n", (int)TY_FLOAT);
    buf_printf(out, "        case %d: return \"cstr\";\n",  (int)TY_CSTR);
    buf_printf(out, "        case %d: return \"ptr\";\n",   (int)TY_PTR_VOID);
    buf_printf(out, "        case %d: return \"struct\";\n", (int)TY_STRUCT);
    buf_printf(out, "        case %d: return \"adt\";\n",    (int)TY_ADT);
    buf_puts(out, "        default: return \"unknown\";\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n");
    /* TY2.3: checked-downcast helper.  (cast x T) verifies the box tag equals
     * the target TypeKind and panics on mismatch (the agreed failure behavior).
     * Declared after tur_panic in the preamble; forward-declare tur_panic here. */
    buf_puts(out, "static void tur_panic(const char *msg);\n");
    buf_puts(out, "static void __tur_any_cast_check(int64_t have, int64_t want) {\n");
    buf_puts(out, "    if (have != want) {\n");
    buf_puts(out, "        char __m[128];\n");
    buf_puts(out, "        snprintf(__m, sizeof(__m), \"cast: any holds %s, not %s\",\n");
    buf_puts(out, "                 __tur_any_type_name(have), __tur_any_type_name(want));\n");
    buf_puts(out, "        tur_panic(__m);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n");
    /* Phase HRT2: existential type — opaque void* wrapping any boxed value */
    buf_puts(out, "/* Phase HRT2: existential type (opaque void* box) */\n");
    buf_puts(out, "typedef void * tur_exists_t;\n");
    /* Phase EX1e / EXG1: heap layout for constrained existentials.
     * Unconstrained `(exists [a] T)` values still flow as plain
     * `tur_exists_t` (an int64_t reinterpreted as void*), unchanged from
     * HRT2.  Constrained `(exists [a] [(C a) ...] T)` values are pointers
     * to a `tur_existential_t` record that bundles the boxed value with
     * one vtable pointer per constraint.  The witnesses array is laid
     * out in the same order as the constraints on the existential type.
     * EXG1: the record lives inline in an RcControlBlock payload (flexible
     * array member for the witnesses) so a single rc_cb_alloc covers both
     * the record and its witnesses; the wrapping control block is what
     * `tur_exists_t` actually points to at runtime. */
    buf_puts(out, "/* Phase EX1e/EXG1: constrained-existential heap record */\n");
    buf_puts(out, "typedef struct tur_existential {\n");
    buf_puts(out, "    int64_t  value;\n");
    buf_puts(out, "    int32_t  n_witnesses;\n");
    buf_puts(out, "    void    *witnesses[];   /* flexible array; length = n_witnesses */\n");
    buf_puts(out, "} tur_existential_t;\n");
    /* EXG1-2: drop hook for rc-managed existential records.  The payload
     * sits inline in the RcControlBlock allocation, so freeing the block
     * itself (via free(cb) in rc_free_queue_drain) reclaims everything;
     * the witnesses array stores stable pointers into static dict
     * singletons and never needs disposal.  This hook is therefore a
     * no-op — it just suppresses the default `free(value)` path that
     * would otherwise double-free the inline payload. */
    buf_puts(out, "/* EXG1-2: drop hook for constrained-existential rc records */\n");
    buf_puts(out, "static void tur_existential_drop(void *value) { (void)value; }\n");
    /* Inline STM runtime - Phase 21 with per-TVar locking */
    buf_puts(out, "/* STM types (Phase 21) */\n");
    buf_puts(out, "typedef void *(*stm_fn_t)(void *env);\n");
    buf_puts(out, "typedef struct TVar { void *value; uint64_t version; pthread_mutex_t lock; pthread_cond_t cond; } TVar;\n");
    buf_puts(out, "typedef struct STM_Transaction STM_Transaction;\n");
    buf_puts(out, "struct STM_Transaction {\n");
    buf_puts(out, "    TVar *read_set[256];\n");
    buf_puts(out, "    uint64_t read_versions[256];\n");
    buf_puts(out, "    int read_count;\n");
    buf_puts(out, "    TVar *write_set[128];\n");
    buf_puts(out, "    void *new_values[128];\n");
    buf_puts(out, "    int write_count;\n");
    buf_puts(out, "    bool retry_requested;\n");
    buf_puts(out, "    bool aborted;\n");
    buf_puts(out, "    bool committed;\n");
    buf_puts(out, "};\n");
    buf_puts(out, "static __thread STM_Transaction *__stm_current_tx = NULL;\n");
    buf_puts(out, "STM_Transaction *tur_stm_current_tx(void) { return __stm_current_tx; }\n");
    buf_puts(out, "void tur_stm_set_current_tx(STM_Transaction *tx) { __stm_current_tx = tx; }\n");
    /* Lock ordering helper */
    buf_puts(out, "static int __stm_lock_cmp(const void *a, const void *b) {\n");
    buf_puts(out, "    const TVar *tv_a = *(const TVar **)a;\n");
    buf_puts(out, "    const TVar *tv_b = *(const TVar **)b;\n");
    buf_puts(out, "    if (tv_a < tv_b) return -1;\n");
    buf_puts(out, "    if (tv_a > tv_b) return 1;\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "STM_Transaction *tur_stm_new_transaction(void) {\n");
    buf_puts(out, "    STM_Transaction *tx = calloc(1, sizeof(STM_Transaction));\n");
    buf_puts(out, "    return tx;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "TVar *tur_tvar_new(void *type, void *initial_value) {\n");
    buf_puts(out, "    (void)type; /* unused in emitted code */\n");
    buf_puts(out, "    TVar *tv = malloc(sizeof(TVar));\n");
    buf_puts(out, "    tv->value = initial_value; tv->version = 1;\n");
    buf_puts(out, "    pthread_mutex_init(&tv->lock, NULL);\n");
    buf_puts(out, "    pthread_cond_init(&tv->cond, NULL);\n");
    buf_puts(out, "    return tv;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "void *tur_tvar_read(STM_Transaction *tx, TVar *tv) {\n");
    buf_puts(out, "    for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "        if (tx->write_set[i] == tv) return tx->new_values[i];\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (tx->read_count < 256) {\n");
    buf_puts(out, "        for (int i = 0; i < tx->read_count; i++) {\n");
    buf_puts(out, "            if (tx->read_set[i] == tv) break;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        tx->read_set[tx->read_count] = tv;\n");
    buf_puts(out, "        tx->read_versions[tx->read_count++] = tv->version;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return tv->value;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "void tur_tvar_write(STM_Transaction *tx, TVar *tv, void *value) {\n");
    buf_puts(out, "    for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "        if (tx->write_set[i] == tv) { tx->new_values[i] = value; return; }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Remove from read set if present */\n");
    buf_puts(out, "    for (int i = 0; i < tx->read_count; i++) {\n");
    buf_puts(out, "        if (tx->read_set[i] == tv) {\n");
    buf_puts(out, "            for (int j = i; j < tx->read_count - 1; j++) {\n");
    buf_puts(out, "                tx->read_set[j] = tx->read_set[j+1];\n");
    buf_puts(out, "                tx->read_versions[j] = tx->read_versions[j+1];\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            tx->read_count--;\n");
    buf_puts(out, "            break;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (tx->write_count < 128) {\n");
    buf_puts(out, "        tx->write_set[tx->write_count] = tv;\n");
    buf_puts(out, "        tx->new_values[tx->write_count++] = value;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n");
    /* Validation */
    buf_puts(out, "static bool __tur_stm_validate(STM_Transaction *tx) {\n");
    buf_puts(out, "    for (int i = 0; i < tx->read_count; i++) {\n");
    buf_puts(out, "        if (tx->read_set[i]->version != tx->read_versions[i]) return false;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return true;\n");
    buf_puts(out, "}\n");
    /* Commit with per-TVar locking (Phase 21) */
    buf_puts(out, "bool tur_stm_commit(STM_Transaction *tx) {\n");
    buf_puts(out, "    /* Sort write set by address for lock ordering */\n");
    buf_puts(out, "    qsort(tx->write_set, tx->write_count, sizeof(TVar *), __stm_lock_cmp);\n");
    buf_puts(out, "    /* Acquire locks in address order */\n");
    buf_puts(out, "    for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "        pthread_mutex_lock(&tx->write_set[i]->lock);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Validate */\n");
    buf_puts(out, "    if (!__tur_stm_validate(tx)) {\n");
    buf_puts(out, "        for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "            pthread_mutex_unlock(&tx->write_set[i]->lock);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Apply writes */\n");
    buf_puts(out, "    for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "        tx->write_set[i]->value = tx->new_values[i];\n");
    buf_puts(out, "        tx->write_set[i]->version++;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Notify waiters */\n");
    buf_puts(out, "    for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "        pthread_cond_broadcast(&tx->write_set[i]->cond);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tx->committed = true;\n");
    buf_puts(out, "    /* Release locks in reverse order */\n");
    buf_puts(out, "    for (int i = tx->write_count - 1; i >= 0; i--) {\n");
    buf_puts(out, "        pthread_mutex_unlock(&tx->write_set[i]->lock);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return true;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "void tur_stm_retry(STM_Transaction *tx) { tx->retry_requested = true; }\n");
    buf_puts(out, "void tur_stm_check(bool condition) { if (!condition) tur_stm_retry(tur_stm_current_tx()); }\n");
    buf_puts(out, "/* Retry with blocking on condition variables (Phase 21) */\n");
    buf_puts(out, "static bool __tur_stm_should_retry(STM_Transaction *tx) {\n");
    buf_puts(out, "    if (tx->retry_requested) return true;\n");
    buf_puts(out, "    if (tx->aborted) return true;\n");
    buf_puts(out, "    return false;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "void *tur_atomically(void *(*fn)(void *), void *env) {\n");
    buf_puts(out, "    STM_Transaction *tx = tur_stm_new_transaction();\n");
    buf_puts(out, "    STM_Transaction *prev = tur_stm_current_tx();\n");
    buf_puts(out, "    while (1) {\n");
    buf_puts(out, "        tx->retry_requested = false;\n");
    buf_puts(out, "        tx->aborted = false;\n");
    buf_puts(out, "        tx->read_count = 0;\n");
    buf_puts(out, "        tx->write_count = 0;\n");
    buf_puts(out, "        tur_stm_set_current_tx(tx);\n");
    buf_puts(out, "        fn(env);\n");
    buf_puts(out, "        if (__tur_stm_should_retry(tx)) {\n");
    buf_puts(out, "            tur_stm_set_current_tx(prev);\n");
    buf_puts(out, "            /* Block on first read TVar's condition variable if retry requested */\n");
    buf_puts(out, "            if (tx->retry_requested && tx->read_count > 0) {\n");
    buf_puts(out, "                TVar *tv = tx->read_set[0];\n");
    buf_puts(out, "                pthread_mutex_lock(&tv->lock);\n");
    buf_puts(out, "                while (tx->retry_requested) {\n");
    buf_puts(out, "                    pthread_cond_wait(&tv->cond, &tv->lock);\n");
    buf_puts(out, "                }\n");
    buf_puts(out, "                pthread_mutex_unlock(&tv->lock);\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            continue;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        if (tur_stm_commit(tx)) {\n");
    buf_puts(out, "            tur_stm_set_current_tx(prev);\n");
    buf_puts(out, "            void *ret = tx->write_count > 0 ? tx->new_values[tx->write_count - 1] : NULL;\n");
    buf_puts(out, "            free(tx);\n");
    buf_puts(out, "            return ret;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        /* Commit failed - retry */\n");
    buf_puts(out, "        tur_stm_set_current_tx(prev);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n");
    /* Phase T21: ucontext.h was moved to the very top (before setjmp.h/pthread.h)
     * to prevent macOS include-guard aliasing of the small 56-byte ucontext_t.
     * Nothing to emit here any more. */
    /* Phase 5/9/M5/19: stdlib.h and string.h are included above; no need for
     * explicit extern declarations of malloc/calloc/free/abort/atexit/memset/
     * memmove/memcpy/strcmp — they are provided by the standard headers. */
    /* Phase T24: Headers for timer wheel, async I/O, and networking.
     * sys/select.h, sys/socket.h, netinet/in.h, arpa/inet.h were moved to the
     * very top (before ucontext.h) to prevent BSD-extension suppression. */
    buf_puts(out, "#include <time.h>\n");
    buf_puts(out, "#include <unistd.h>\n");
    buf_puts(out, "#include <fcntl.h>\n");
    buf_puts(out, "#include <errno.h>\n");
    buf_puts(out, "\n");
    /* Phase 7 follow-up: minimal in-process test registry for stdlib/test.tur. */
    buf_puts(out, "#define TUR_TEST_REGISTRY_MAX 1024\n");
    /* Phase B5: backtrack depth cap (0 = unlimited) */
    buf_printf(out, "#define BACKTRACK_DEPTH_DEFAULT %lld\n", (long long)g_backtrack_depth);
    buf_puts(out, "typedef int64_t (*tur_test_callback_t)(void);\n");
    buf_puts(out, "static const char *tur_test_registry_names[TUR_TEST_REGISTRY_MAX];\n");
    buf_puts(out, "static tur_test_callback_t tur_test_registry_fns[TUR_TEST_REGISTRY_MAX];\n");
    buf_puts(out, "static int64_t tur_test_registry_count = 0;\n\n");
    buf_puts(out, "int64_t tur_test_register(const char *name, void *test_fn) {\n");
    buf_puts(out, "    if (!test_fn) return 0;\n");
    buf_puts(out, "    if (tur_test_registry_count >= TUR_TEST_REGISTRY_MAX) return 0;\n");
    buf_puts(out, "    tur_test_registry_names[tur_test_registry_count] = name ? name : \"<unnamed>\";\n");
    buf_puts(out, "    tur_test_registry_fns[tur_test_registry_count] = (tur_test_callback_t)test_fn;\n");
    buf_puts(out, "    tur_test_registry_count++;\n");
    buf_puts(out, "    return 1;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "int64_t tur_test_run_all(void) {\n");
    buf_puts(out, "    int64_t passed = 0;\n");
    buf_puts(out, "    int64_t failed = 0;\n");
    buf_puts(out, "    for (int64_t i = 0; i < tur_test_registry_count; i++) {\n");
    buf_puts(out, "        tur_test_callback_t fn = tur_test_registry_fns[i];\n");
    buf_puts(out, "        int64_t rc = fn ? fn() : 0;\n");
    buf_puts(out, "        if (rc == 1) {\n");
    buf_puts(out, "            putchar('.');\n");
    buf_puts(out, "            putchar('\\n');\n");
    buf_puts(out, "            passed++;\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            putchar('F');\n");
    buf_puts(out, "            putchar('\\n');\n");
    buf_puts(out, "            printf(\"%s\\n\", tur_test_registry_names[i]);\n");
    buf_puts(out, "            failed++;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    printf(\"summary: %lld passed, %lld failed\\n\",\n");
    buf_puts(out, "           (long long)passed, (long long)failed);\n");
    buf_puts(out, "    return failed == 0 ? 0 : 1;\n");
    buf_puts(out, "}\n\n");
    /* Phase 4 v1 lowering: emit tur_frame inline from runtime.h */
    buf_puts(out, "/* tur_frame - phase 4 v1 lowering (from runtime.h) */\n");
    buf_puts(out, "typedef void (*defer_fn_t)(void *env);\n");
    buf_puts(out, "#define TUR_FRAME_MAX_DEFERS 32\n\n");
    buf_puts(out, "typedef struct tur_frame {\n");
    buf_puts(out, "    defer_fn_t defers[TUR_FRAME_MAX_DEFERS];\n");
    buf_puts(out, "    void *envs[TUR_FRAME_MAX_DEFERS];\n");
    buf_puts(out, "    int n;\n");
    buf_puts(out, "    struct tur_frame *parent;\n");
    buf_puts(out, "    bool may_capture;\n");
    buf_puts(out, "} tur_frame;\n\n");
    buf_puts(out, "static inline void tur_frame_init(tur_frame *f, tur_frame *parent) {\n");
    buf_puts(out, "    f->n = 0; f->parent = parent; f->may_capture = false;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static inline int tur_frame_push_defer(tur_frame *f, defer_fn_t thunk, void *env) {\n");
    buf_puts(out, "    if (f->n >= TUR_FRAME_MAX_DEFERS) return -1;\n");
    buf_puts(out, "    f->defers[f->n] = thunk;\n");
    buf_puts(out, "    f->envs[f->n] = env;\n");
    buf_puts(out, "    f->n++;\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void tur_frame_fire_lifo(tur_frame *f) {\n");
    buf_puts(out, "    for (int i = f->n - 1; i >= 0; i--) f->defers[i](f->envs[i]);\n");
    buf_puts(out, "    f->n = 0;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void tur_frame_fire_chain(tur_frame *f) {\n");
    buf_puts(out, "    tur_frame *frames[64];\n");
    buf_puts(out, "    int n_frames = 0;\n");
    buf_puts(out, "    for (tur_frame *cur = f; cur != NULL && n_frames < 64; cur = cur->parent) {\n");
    buf_puts(out, "        frames[n_frames++] = cur;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    for (int i = n_frames - 1; i >= 0; i--) {\n");
    buf_puts(out, "        tur_frame_fire_lifo(frames[i]);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    
    /* Phase R2: tur_panic - integrated with defer chain */
    /* Phase R6: Add g_panic_trace flag for scope chain printing */
    buf_puts(out, "/* Phase R2/R6: tur_panic */\n");
    buf_puts(out, "static int tur_panic_in_progress = 0;\n");
    buf_puts(out, "static tur_frame *global_panic_frame = NULL;\n");
    buf_puts(out, "static int g_panic_trace = 0;  /* Set by compiler when --panic-trace is used */\n");
    /* CLI-ARGS: g_tur_args holds the *args* list (linked list of argv strings, built in main). */
    buf_puts(out, "static int64_t g_tur_args = 0;  /* *args*: CLI arguments as list of :cstr (set in main) */\n");
    buf_puts(out, "static void tur_panic_set_frame(tur_frame *f) {\n");
    buf_puts(out, "    global_panic_frame = f;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void tur_panic_print_scope_chain(void) {\n");
    buf_puts(out, "    if (!g_panic_trace || !global_panic_frame) return;\n");
    buf_puts(out, "    fprintf(stderr, \"  scope chain:\\n\");\n");
    buf_puts(out, "    tur_frame *frames[64];\n");
    buf_puts(out, "    int n_frames = 0;\n");
    buf_puts(out, "    for (tur_frame *cur = global_panic_frame; cur != NULL && n_frames < 64; cur = cur->parent) {\n");
    buf_puts(out, "        frames[n_frames++] = cur;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    for (int i = 0; i < n_frames; i++) {\n");
    buf_puts(out, "        fprintf(stderr, \"    at frame %p (parent: %p, n_defers: %d)\\n\",\n");
    buf_puts(out, "                (void*)frames[i], (void*)frames[i]->parent, frames[i]->n);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    /* Phase R2: forward decls so plain tur_panic can unwind to a catch-unwind
     * boundary (the payload machinery itself is emitted further below). */
    buf_puts(out, "typedef struct tur_panic_payload tur_panic_payload;\n");
    buf_puts(out, "static jmp_buf global_panic_jmpbuf;\n");
    buf_puts(out, "static int global_panic_jmpbuf_valid;\n");
    buf_puts(out, "static tur_panic_payload *global_panic_payload;\n");
    buf_puts(out, "static tur_panic_payload *panic_payload_new(int, void *, const char *, int);\n");
    buf_puts(out, "static void tur_panic(const char *msg) {\n");
    buf_puts(out, "    if (tur_panic_in_progress) {\n");
    buf_puts(out, "        fprintf(stderr, \"double panic: aborting\\n\");\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tur_panic_in_progress = 1;\n");
    /* Phase R2: if a catch-unwind boundary is active, box the message as a
     * :cstr payload, fire the panicking frame's defers, and longjmp to it.
     * The defer chain stops at this function's frame tree (the catch boundary
     * lives in a different call frame), giving partial unwind for free. */
    buf_printf(out, "    if (global_panic_jmpbuf_valid) {\n");
    buf_printf(out, "        global_panic_payload = panic_payload_new(%d, msg ? strdup(msg) : NULL, __FILE__, __LINE__);\n", (int)TY_CSTR);
    buf_puts(out, "        if (global_panic_frame) { tur_frame_fire_chain(global_panic_frame); }\n");
    buf_puts(out, "        longjmp(global_panic_jmpbuf, 1);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    fprintf(stderr, \"panic at %s:%d: %s\\n\", __FILE__, __LINE__, msg ? msg : \"(no message)\");\n");
    buf_puts(out, "    tur_panic_print_scope_chain();\n");
    buf_puts(out, "    if (global_panic_frame) {\n");
    buf_puts(out, "        tur_frame_fire_chain(global_panic_frame);\n");
    buf_puts(out, "    }\n");
    /* Flush buffered output (defers fired above may have printed) before
     * abort(), which does not flush stdio streams. */
    buf_puts(out, "    fflush(NULL);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");

    /* Phase R5: tur_panic_abort for #[no-unwind] */
    buf_puts(out, "/* Phase R5: tur_panic_abort - no unwinding, immediate abort */\n");
    buf_puts(out, "static void tur_panic_abort(const char *msg) {\n");
    buf_puts(out, "    fprintf(stderr, \"panic (no unwind): %s\\n\", msg ? msg : \"(no message)\");\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");

    /* CPS3: emit tur_cps_cont_t + tur_cps_apply when --cps-path is active */
    if (g_cps_path) {
        buf_puts(out, "/* CPS3: tur_cps_cont_t -- v1 identity-CPS continuation handle */\n");
        buf_puts(out, "typedef struct tur_cps_cont {\n");
        buf_puts(out, "    void (*fn)(struct tur_cps_cont *k, int64_t value);\n");
        buf_puts(out, "} tur_cps_cont_t;\n");
        buf_puts(out, "static inline void tur_cps_apply(tur_cps_cont_t *k, int64_t v) { if (k) k->fn(k, v); }\n\n");
    }

    /* Phase R2: Panic with typed payload */
    /* tur_panic_with is forward-declared here; its body is emitted after
     * FiberBlock in Phase T21 so it can dereference tur_current_fiber. */
    buf_puts(out, "static void tur_panic_with(int type_tag, void *payload, const char *file, int line);\n\n");
    buf_puts(out, "/* Phase R2: tur_panic_with types */\n");
    buf_puts(out, "struct tur_panic_payload {\n");
    buf_puts(out, "    int type_tag;\n");
    buf_puts(out, "    void *value;\n");
    buf_puts(out, "    const char *file;\n");
    buf_puts(out, "    int line;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static tur_panic_payload *global_panic_payload = NULL;\n");
    buf_puts(out, "static jmp_buf global_panic_jmpbuf;\n");
    buf_puts(out, "static int global_panic_jmpbuf_valid = 0;\n\n");
    buf_puts(out, "static tur_panic_payload *panic_payload_new(int type_tag, void *payload, const char *file, int line) {\n");
    buf_puts(out, "    tur_panic_payload *p = (tur_panic_payload *)malloc(sizeof(tur_panic_payload));\n");
    buf_puts(out, "    if (!p) { fprintf(stderr, \"panic: oom\\n\"); abort(); }\n");
    buf_puts(out, "    p->type_tag = type_tag; p->value = payload; p->file = file; p->line = line;\n");
    buf_puts(out, "    return p;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void panic_payload_free(tur_panic_payload *p) {\n");
    buf_puts(out, "    if (p) { free(p->value); free(p); }\n");
    buf_puts(out, "}\n\n");
    /* Phase R2: Panic payload accessors */
    buf_puts(out, "static int tur_panic_payload_type(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->type_tag : 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void *tur_panic_payload_value(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->value : NULL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static const char *tur_panic_payload_file(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->file : NULL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int tur_panic_payload_line(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->line : 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void *tur_panic_payload_downcast(tur_panic_payload *p, int target_type) {\n");
    buf_puts(out, "    if (!p || p->type_tag != target_type) return NULL;\n");
    buf_puts(out, "    return p->value;\n");
    buf_puts(out, "}\n\n");
    /* Phase R2: catch-unwind and catch-panic-of */
    buf_puts(out, "/* Phase R2: catch-unwind/catch-panic-of */\n");
    buf_puts(out, "typedef enum { TUR_RESULT_OK, TUR_RESULT_ERR } tur_result_tag;\n");
    buf_puts(out, "typedef struct tur_result tur_result;\n");
    buf_puts(out, "struct tur_result {\n");
    buf_puts(out, "    tur_result_tag tag;\n");
    buf_puts(out, "    union { int64_t ok_val; void *ok_ptr; tur_panic_payload *err; } u;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "typedef void (*tur_thunk_fn)(void *env, tur_result *out);\n\n");
    buf_puts(out, "static bool tur_catch_unwind(tur_thunk_fn thunk, void *env, tur_result *out) {\n");
    buf_puts(out, "    if (global_panic_jmpbuf_valid) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    global_panic_jmpbuf_valid = 1;\n");
    buf_puts(out, "    if (setjmp(global_panic_jmpbuf) == 0) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        if (global_panic_payload) {\n");
    buf_puts(out, "            panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "            global_panic_payload = NULL;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        tur_panic_in_progress = 0;\n");
    buf_puts(out, "        out->tag = TUR_RESULT_ERR;\n");
    buf_puts(out, "        out->u.err = global_panic_payload;\n");
    buf_puts(out, "        global_panic_payload = NULL;\n");
    buf_puts(out, "        return true;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static bool tur_catch_panic_of(int expected_type, tur_thunk_fn thunk, void *env, tur_result *out) {\n");
    buf_puts(out, "    if (global_panic_jmpbuf_valid) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    global_panic_jmpbuf_valid = 1;\n");
    buf_puts(out, "    if (setjmp(global_panic_jmpbuf) == 0) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        if (global_panic_payload) {\n");
    buf_puts(out, "            panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "            global_panic_payload = NULL;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        tur_panic_in_progress = 0;\n");
    buf_puts(out, "        if (global_panic_payload && global_panic_payload->type_tag == expected_type) {\n");
    buf_puts(out, "            out->tag = TUR_RESULT_ERR;\n");
    buf_puts(out, "            out->u.err = global_panic_payload;\n");
    buf_puts(out, "            global_panic_payload = NULL;\n");
    buf_puts(out, "            return true;\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "        if (global_panic_payload) {\n");
    buf_puts(out, "            /* Type mismatch - re-panic */\n");
    buf_puts(out, "            tur_panic_with(global_panic_payload->type_tag, global_panic_payload->value,\n");
    buf_puts(out, "                           global_panic_payload->file, global_panic_payload->line);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    /* Phase R2: catch-unwind/catch-panic-of for the (catch-unwind thunk) special
     * form.  Unlike the try/catch helpers above (purpose-built tur_thunk_fn ABI),
     * these take a fat-closure thunk (int64_t handle) invoked via TUR_APPLY0 and
     * return a result box (tur_ok / tur_err) so the value composes with
     * err?/ok?/ok-val/err-val and the ? operator.  The single global jmp_buf is
     * saved/restored on entry/exit so nested boundaries work.  The caught panic
     * payload becomes the err value (an opaque Panic handle); it is intentionally
     * not freed here -- ownership passes to the returned result. */
    buf_puts(out, "/* Phase R2: catch-unwind special-form helpers (result-box ABI) */\n");
    buf_puts(out, "static int64_t tur_catch_unwind_box(int64_t thunk) {\n");
    buf_puts(out, "    jmp_buf __prev_buf; int __prev_valid = global_panic_jmpbuf_valid;\n");
    buf_puts(out, "    if (__prev_valid) memcpy(&__prev_buf, &global_panic_jmpbuf, sizeof(jmp_buf));\n");
    buf_puts(out, "    global_panic_jmpbuf_valid = 1;\n");
    buf_puts(out, "    if (setjmp(global_panic_jmpbuf) == 0) {\n");
    buf_puts(out, "        int64_t __v = TUR_APPLY0(thunk);\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = __prev_valid;\n");
    buf_puts(out, "        if (__prev_valid) memcpy(&global_panic_jmpbuf, &__prev_buf, sizeof(jmp_buf));\n");
    buf_puts(out, "        return tur_ok(__v);\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = __prev_valid;\n");
    buf_puts(out, "        if (__prev_valid) memcpy(&global_panic_jmpbuf, &__prev_buf, sizeof(jmp_buf));\n");
    buf_puts(out, "        tur_panic_in_progress = 0;\n");
    buf_puts(out, "        tur_panic_payload *__p = global_panic_payload;\n");
    buf_puts(out, "        global_panic_payload = NULL;\n");
    buf_puts(out, "        return tur_err((int64_t)(intptr_t)__p);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int64_t tur_catch_panic_of_box(int expected_type, int64_t thunk) {\n");
    buf_puts(out, "    jmp_buf __prev_buf; int __prev_valid = global_panic_jmpbuf_valid;\n");
    buf_puts(out, "    if (__prev_valid) memcpy(&__prev_buf, &global_panic_jmpbuf, sizeof(jmp_buf));\n");
    buf_puts(out, "    global_panic_jmpbuf_valid = 1;\n");
    buf_puts(out, "    if (setjmp(global_panic_jmpbuf) == 0) {\n");
    buf_puts(out, "        int64_t __v = TUR_APPLY0(thunk);\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = __prev_valid;\n");
    buf_puts(out, "        if (__prev_valid) memcpy(&global_panic_jmpbuf, &__prev_buf, sizeof(jmp_buf));\n");
    buf_puts(out, "        return tur_ok(__v);\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = __prev_valid;\n");
    buf_puts(out, "        if (__prev_valid) memcpy(&global_panic_jmpbuf, &__prev_buf, sizeof(jmp_buf));\n");
    buf_puts(out, "        tur_panic_in_progress = 0;\n");
    buf_puts(out, "        tur_panic_payload *__p = global_panic_payload;\n");
    buf_puts(out, "        global_panic_payload = NULL;\n");
    buf_puts(out, "        if (__p && __p->type_tag == expected_type) {\n");
    buf_puts(out, "            return tur_err((int64_t)(intptr_t)__p);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        /* type mismatch: re-raise to the next outer boundary (restored above) */\n");
    buf_puts(out, "        if (__p) tur_panic_with(__p->type_tag, __p->value, __p->file, __p->line);\n");
    buf_puts(out, "        return tur_err(0);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    /* Phase B2 / MS1: Cloneable continuation runtime (inline in generated C).
     * Emitted when the program uses cloneable-shift/reset OR any ^multishot handler
     * (which uses tur_cloneable_cont wrappers + tur_continuation_snapshot), or when
     * a cloneable-reset lowers onto the DK machine (CPS9): the DK bridge wraps the
     * captured context in a tur_cloneable_cont. (cps_expr_contains_cloneable_shift
     * does not look inside builtins, so a context-nested shift needs this gate.) */
    if (cps_expr_contains_cloneable_shift(program) || expr_has_multishot_handler(program) ||
        emit_cps_program_uses_cloneable_dk(program)) {
    buf_puts(out, "/* Phase B2: Cloneable continuation runtime */\n");
    buf_puts(out, "typedef struct tur_cloneable_cont tur_cloneable_cont;\n");
    buf_puts(out, "struct tur_cloneable_cont {\n");
    buf_puts(out, "    int64_t (*cont_fn)(void *env, int64_t value);\n");
    buf_puts(out, "    void *env;\n");
    buf_puts(out, "    void *(*clone_env)(const void *env);\n");
    buf_puts(out, "    void (*drop_env)(void *env);\n");
    buf_puts(out, "};\n\n");
    /* Internal allocator — takes explicit function pointers */
    buf_puts(out, "static tur_cloneable_cont *tur_cloneable_cont_alloc(\n");
    buf_puts(out, "    int64_t (*cont_fn)(void *, int64_t),\n");
    buf_puts(out, "    void *env,\n");
    buf_puts(out, "    void *(*clone_env)(const void *),\n");
    buf_puts(out, "    void (*drop_env)(void *)) {\n");
    buf_puts(out, "    tur_cloneable_cont *c = malloc(sizeof(tur_cloneable_cont));\n");
    buf_puts(out, "    if (!c) abort();\n");
    buf_puts(out, "    c->cont_fn = cont_fn; c->env = env;\n");
    buf_puts(out, "    c->clone_env = clone_env; c->drop_env = drop_env;\n");
    buf_puts(out, "    return c;\n");
    buf_puts(out, "}\n\n");
    /* User-facing: resume — cont is an opaque int64_t (pointer cast) */
    buf_puts(out, "static int64_t tur_cloneable_cont_resume(int64_t cont_int, int64_t value) {\n");
    buf_puts(out, "    tur_cloneable_cont *cont = (tur_cloneable_cont *)(intptr_t)cont_int;\n");
    buf_puts(out, "    if (!cont || !cont->cont_fn) abort();\n");
    buf_puts(out, "    return cont->cont_fn(cont->env, value);\n");
    buf_puts(out, "}\n\n");
    /* User-facing: clone — deep-copy env via clone_env function pointer */
    buf_puts(out, "static int64_t tur_cloneable_cont_clone(int64_t cont_int) {\n");
    buf_puts(out, "    tur_cloneable_cont *cont = (tur_cloneable_cont *)(intptr_t)cont_int;\n");
    buf_puts(out, "    if (!cont) return 0;\n");
    buf_puts(out, "    tur_cloneable_cont *copy = malloc(sizeof(tur_cloneable_cont));\n");
    buf_puts(out, "    if (!copy) abort();\n");
    buf_puts(out, "    copy->cont_fn   = cont->cont_fn;\n");
    buf_puts(out, "    copy->clone_env = cont->clone_env;\n");
    buf_puts(out, "    copy->drop_env  = cont->drop_env;\n");
    buf_puts(out, "    copy->env = cont->clone_env ? cont->clone_env(cont->env) : cont->env;\n");
    buf_puts(out, "    return (int64_t)(intptr_t)copy;\n");
    buf_puts(out, "}\n\n");
    /* MS0: tur_continuation_snapshot — alias for clone; used by ^multishot resume */
    buf_puts(out, "#define tur_continuation_snapshot tur_cloneable_cont_clone\n\n");
    /* User-facing: drop — fire drop_env, then free struct */
    buf_puts(out, "static void tur_cloneable_cont_drop(int64_t cont_int) {\n");
    buf_puts(out, "    tur_cloneable_cont *cont = (tur_cloneable_cont *)(intptr_t)cont_int;\n");
    buf_puts(out, "    if (!cont) return;\n");
    buf_puts(out, "    if (cont->env && cont->drop_env) cont->drop_env(cont->env);\n");
    buf_puts(out, "    free(cont);\n");
    buf_puts(out, "}\n\n");
    /* CPS-CL4: Reset context for setjmp/longjmp-based cloneable continuations.
     * Each cloneable-reset pushes a context on this thread-local stack;
     * cloneable-shift stores k_fn's return value and longjmps back. */
    buf_puts(out, "/* CPS-CL4: cloneable-reset context */\n");
    buf_puts(out, "typedef struct tur_cloneable_reset_ctx {\n");
    buf_puts(out, "    jmp_buf jmp;\n");
    buf_puts(out, "    int64_t result;  /* k_fn return value, set by shift before longjmp */\n");
    buf_puts(out, "    struct tur_cloneable_reset_ctx *prev; /* for nested resets */\n");
    buf_puts(out, "} tur_cloneable_reset_ctx;\n\n");
    buf_puts(out, "static __thread tur_cloneable_reset_ctx *tur_current_reset_ctx = NULL;\n\n");

    } /* end if (cps_expr_contains_cloneable_shift(program)) */

    /* cps-transform-plan: emit the heap-reified CPS substrate (DK multi-prompt
     * machine) when the program uses base delimited control (reset/shift/
     * shift0), or when a cloneable-reset lowers onto the DK machine (CPS9).
     * emit_cps_reset / emit_cps_cloneable_reset lower onto dk_run/dk_shift. */
    if (emit_cps_program_uses_delimited(program) ||
        emit_cps_program_uses_cloneable_dk(program) ||
        emit_cps_program_uses_serial_dk(program)) {
        emit_cps_runtime_prelude(out);
    }
    /* CPS9: the cloneable-continuation <-> DK bridge needs both the cloneable
     * runtime (emitted above) and the DK machine (just emitted) in scope. */
    if (emit_cps_program_uses_cloneable_dk(program)) {
        emit_cps_cloneable_bridge_prelude(out);
    }
    /* CPS10 (CPS5.4): the serial marshaling runtime needs the DK machine. */
    if (emit_cps_program_uses_serial_dk(program)) {
        emit_cps_serial_runtime_prelude(out);
    }

    /* call-cc-completion: emit the undelimited escape-continuation runtime when
     * the program uses (call/cc f) / (escape f). */
    if (emit_cps_program_uses_callcc(program)) {
        emit_cps_callcc_prelude(out);
    }

    /* Phase 19: Effect handler chain */
    buf_puts(out, "/* Phase 19: Algebraic effect handler chain */\n");
    /* Phase P19-5: TurContK — a lightweight per-invocation continuation token.
     * Each tur_effect_perform call allocates one on the stack and passes its
     * address as `k` to the handler function.  `consumed` is set by (resume k v)
     * so that (cont? k) can verify freshness at runtime. */
    buf_puts(out, "typedef struct { bool consumed; void *origin_fiber; } TurContK;\n\n");
    /* Phase 19D: Effect-capture continuation context */
    buf_puts(out, "/* Phase 19D: Effect-capture continuation context */\n");
    buf_puts(out, "typedef struct TurEffectCaptureCtx TurEffectCaptureCtx;\n");
    buf_puts(out, "struct TurEffectCaptureCtx {\n");
    buf_puts(out, "    bool has_pending_effect;\n");
    buf_puts(out, "    const char *eff_name;\n");
    buf_puts(out, "    int64_t eff_args[8];  /* v1: max 8 args */\n");
    buf_puts(out, "    int eff_n_args;\n");
    buf_puts(out, "    int64_t (*dispatch)(void *ctx, int64_t k, int64_t v);\n");
    buf_puts(out, "    void *body_env;  /* heap-allocated env for body captures */\n");
    buf_puts(out, "    void *table;     /* FH3: tur_handler_table_t* for value-based dispatch (else NULL) */\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "typedef struct EffectHandlerCase EffectHandlerCase;\n");
    buf_puts(out, "struct EffectHandlerCase {\n");
    buf_puts(out, "    const char *effect_name;\n");
    buf_puts(out, "    int64_t (*handler_fn)(int64_t *args, int n_args, int64_t k, void *env);\n");
    buf_puts(out, "    void *env;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "typedef struct EffectHandlerFrame EffectHandlerFrame;\n");
    buf_puts(out, "struct EffectHandlerFrame {\n");
    buf_puts(out, "    struct EffectHandlerFrame *parent;\n");
    buf_puts(out, "    int n_cases;\n");
    buf_puts(out, "    EffectHandlerCase cases[8];\n");
    buf_puts(out, "};\n\n");
        /* Phase T21-A/B / P19-8: FiberBlock — cooperative fiber runtime via ucontext_t.
     * tur_current_fiber is thread-local; set/restored by tur_fiber_block_resume.
     * tur_effect_perform checks tur_current_fiber->effect_handler_chain for fiber-local
     * effect handler chains (Phase T21-B / P19-8). */
    buf_puts(out, "/* Phase T21: FiberBlock */\n");
    buf_puts(out, "#ifdef __clang__\n");
    buf_puts(out, "#pragma clang diagnostic push\n");
    buf_puts(out, "#pragma clang diagnostic ignored \"-Wdeprecated-declarations\"\n");
    buf_puts(out, "#endif\n");
    buf_puts(out, "typedef struct FiberBlock FiberBlock;\n");
    buf_puts(out, "struct FiberBlock {\n");
    buf_puts(out, "    ucontext_t ctx;\n");
    buf_puts(out, "    ucontext_t caller_ctx;\n");
    buf_puts(out, "    char *stack;\n");
    buf_puts(out, "    size_t stack_size;\n");
    buf_puts(out, "    int done;\n");
    buf_puts(out, "    int parked; /* Phase T21: scheduler park/unpark */\n");
    buf_puts(out, "    int64_t result;\n");
    buf_puts(out, "    int64_t arg;\n");
    buf_puts(out, "    void *effect_handler_chain; /* Phase P19-8: per-fiber effect handler chain */\n");
    buf_puts(out, "    bool migration_safe; /* SCH-004: true if effect handlers are safe for cross-thread migration */\n");
    buf_puts(out, "    void (*entry_fn)(void);\n");
    buf_puts(out, "    void *fiber_local; /* Phase T21: fiber-local storage */\n");
    /* Phase T22: Structured concurrency */
    buf_puts(out, "    void *task_group; /* Parent TaskGroup for cancellation */\n");
    buf_puts(out, "    bool cancelled; /* Set when parent TaskGroup is cancelled */\n");
    /* Phase TG-004-1 PR: Per-fiber panic handling for auto-cancel propagation */
    buf_puts(out, "    jmp_buf panic_jmpbuf; /* Per-fiber panic recovery buffer */\n");
    buf_puts(out, "    bool panic_jmpbuf_valid; /* Whether this fiber's panic handler is active */\n");
    buf_puts(out, "    void *eff_ctx;  /* Phase 19D: NULL for regular fibers, TurEffectCaptureCtx* for effect-capture fibers */\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static __thread FiberBlock *tur_current_fiber = NULL;\n");
    /* Phase R2: tur_panic_with body — placed here so FiberBlock and
     * tur_current_fiber are in scope for the per-fiber panic check. */
    buf_puts(out, "static void tur_panic_with(int type_tag, void *payload, const char *file, int line) {\n");
    buf_puts(out, "    if (tur_panic_in_progress) {\n");
    buf_puts(out, "        fprintf(stderr, \"double panic: aborting\\n\");\n");
    buf_puts(out, "        free(payload);\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tur_panic_in_progress = 1;\n");
    /* Phase TG-004-2 PR: Check global handler first (try/catch has priority), then fiber */
    buf_puts(out, "    if (global_panic_jmpbuf_valid) {\n");
    buf_puts(out, "        global_panic_payload = panic_payload_new(type_tag, payload, file, line);\n");
    buf_puts(out, "        longjmp(global_panic_jmpbuf, 1);\n");
    buf_puts(out, "    } else if (tur_current_fiber && tur_current_fiber->panic_jmpbuf_valid) {\n");
    buf_puts(out, "        /* Use per-fiber panic buffer - set up global payload for cleanup */\n");
    buf_puts(out, "        global_panic_payload = panic_payload_new(type_tag, payload, file, line);\n");
    buf_puts(out, "        longjmp(tur_current_fiber->panic_jmpbuf, 1);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    fprintf(stderr, \"panic at %s:%d\\n\", file ? file : \"(unknown)\", line);\n");
    buf_puts(out, "    free(payload);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");
    /* Phase T22: Cooperative cancellation flag */
    buf_puts(out, "static __thread bool tur_fiber_cancelled_flag = false;\n\n");
    /* Phase T22: TaskGroup notification forward declaration */
    buf_puts(out, "static void tur_task_group_notify_done(void *task_group);\n");
    /* Forward-declare tur_fiber_set_cancelled before tur_fiber_shim uses it */
    buf_puts(out, "static void tur_fiber_set_cancelled(bool c);\n\n");
    /* TC0: per-thread cooperative cancellation state for POSIX threads */
    buf_puts(out, "/* TC0: per-thread cancel state for cooperative POSIX-thread cancellation */\n");
    buf_puts(out, "typedef struct {\n");
    buf_puts(out, "    volatile int    cancel_requested;  /* set by canceller via tur_thread_cancel */\n");
    buf_puts(out, "    pthread_mutex_t cancel_mutex;\n");
    buf_puts(out, "    pthread_cond_t  cancel_cond;\n");
    buf_puts(out, "} TurThreadState;\n\n");
    buf_puts(out, "typedef struct {\n");
    buf_puts(out, "    pthread_t        tid;\n");
    buf_puts(out, "    TurThreadState  *state;  /* heap-allocated; owned by this handle */\n");
    buf_puts(out, "} TurThreadHandle;\n\n");
    buf_puts(out, "typedef struct {\n");
    buf_puts(out, "    void           *(*user_fn)(void *);\n");
    buf_puts(out, "    void           *user_arg;\n");
    buf_puts(out, "    TurThreadState *state;\n");
    buf_puts(out, "} TurThreadSpawnArg;\n\n");
    buf_puts(out, "/* TC0: thread-local pointer to this thread's cancel state (NULL on main thread) */\n");
    buf_puts(out, "static __thread TurThreadState *tur_current_thread_state = NULL;\n");
    buf_puts(out, "/* TC0: thread-local setjmp buffer for with-cancel-guard (0 = not active) */\n");
    buf_puts(out, "static __thread jmp_buf tur_cancel_jmpbuf;\n");
    buf_puts(out, "static __thread int tur_cancel_jmpbuf_valid = 0;\n\n");
    buf_puts(out, "static void *tur_thread_trampoline(void *raw) {\n");
    buf_puts(out, "    TurThreadSpawnArg *a = (TurThreadSpawnArg *)raw;\n");
    buf_puts(out, "    tur_current_thread_state = a->state;\n");
    buf_puts(out, "    void *(*fn)(void *) = a->user_fn;\n");
    buf_puts(out, "    void *user_arg = a->user_arg;\n");
    buf_puts(out, "    free(a);\n");
    buf_puts(out, "    return fn(user_arg);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int tur_thread_cancel_requested(void) {\n");
    buf_puts(out, "    TurThreadState *s = tur_current_thread_state;\n");
    buf_puts(out, "    return s ? __atomic_load_n(&s->cancel_requested, __ATOMIC_ACQUIRE) : 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "/* TC0: cancel action -- longjmp into cancel guard if active, else exit thread */\n");
    buf_puts(out, "static void tur_thread_do_cancel(void) {\n");
    buf_puts(out, "    if (tur_cancel_jmpbuf_valid) {\n");
    buf_puts(out, "        tur_cancel_jmpbuf_valid = 0;\n");
    buf_puts(out, "        longjmp(tur_cancel_jmpbuf, 1);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* No cancel guard -- exit the thread cleanly without panicking. */\n");
    buf_puts(out, "    pthread_exit(NULL);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_shim(uint32_t hi, uint32_t lo) {\n");
    buf_puts(out, "    FiberBlock *f = (FiberBlock *)(((uintptr_t)hi << 32) | (uintptr_t)(uint32_t)lo);\n");
    /* Phase TG-004-3 PR: Per-fiber panic handling with auto-cancel on panic */
    buf_puts(out, "    void *task_group = f->task_group;\n");
    buf_puts(out, "    if (task_group) {\n");
    buf_puts(out, "        /* Save previous global panic handler state */\n");
    buf_puts(out, "        int prev_global_valid = global_panic_jmpbuf_valid;\n");
    buf_puts(out, "        jmp_buf prev_global_buf;\n");
    buf_puts(out, "        if (prev_global_valid) memcpy(&prev_global_buf, &global_panic_jmpbuf, sizeof(jmp_buf));\n");
    buf_puts(out, "        /* Clear global to prevent interference */\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        /* Set up per-fiber panic handler */\n");
    buf_puts(out, "        if (setjmp(f->panic_jmpbuf) == 0) {\n");
    buf_puts(out, "            f->panic_jmpbuf_valid = 1;\n");
    buf_puts(out, "            tur_current_fiber = f;\n");
    buf_puts(out, "            f->entry_fn();\n");
    buf_puts(out, "            tur_current_fiber = NULL;\n");
    buf_puts(out, "            f->panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "            if (global_panic_payload) {\n");
    buf_puts(out, "                panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "                global_panic_payload = NULL;\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            /* Restore previous global panic handler */\n");
    buf_puts(out, "            global_panic_jmpbuf_valid = prev_global_valid;\n");
    buf_puts(out, "            if (prev_global_valid) memcpy(&global_panic_jmpbuf, &prev_global_buf, sizeof(jmp_buf));\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            /* Panic caught - auto-cancel task group (TG-004-3) */\n");
    buf_puts(out, "            f->panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "            tur_current_fiber = NULL;\n");
    buf_puts(out, "            if (global_panic_payload) {\n");
    buf_puts(out, "                typedef struct TaskGroupBlock { bool cancelled; bool done; int64_t cancel_reason; pthread_mutex_t lock; pthread_cond_t done_cond; } TaskGroupBlock;\n");
    buf_puts(out, "                TaskGroupBlock *g = (TaskGroupBlock *)task_group;\n");
    buf_puts(out, "                pthread_mutex_lock(&g->lock);\n");
    buf_puts(out, "                g->cancelled = true;\n");
    buf_puts(out, "                g->done = true;\n");
    buf_puts(out, "                g->cancel_reason = 1; /* panic reason (TG-004-2) */\n");
    buf_puts(out, "                pthread_cond_broadcast(&g->done_cond);\n");
    buf_puts(out, "                pthread_mutex_unlock(&g->lock);\n");
    buf_puts(out, "                tur_fiber_set_cancelled(true);\n");
    buf_puts(out, "                panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "                global_panic_payload = NULL;\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            /* Restore previous global panic handler */\n");
    buf_puts(out, "            global_panic_jmpbuf_valid = prev_global_valid;\n");
    buf_puts(out, "            if (prev_global_valid) memcpy(&global_panic_jmpbuf, &prev_global_buf, sizeof(jmp_buf));\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        /* No task group, just run the function normally */\n");
    buf_puts(out, "        f->entry_fn();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    f->done = 1;\n");
    /* Phase T22: Notify task group on completion (TG-002: only if task_group is set) */
    buf_puts(out, "    if (task_group) tur_task_group_notify_done(task_group);\n");
    buf_puts(out, "    swapcontext(&f->ctx, &f->caller_ctx);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");
    /* Declare global_effect_handler_chain before tur_fiber_block_new uses it */
    buf_puts(out, "static __thread EffectHandlerFrame *global_effect_handler_chain = NULL;\n\n");
    buf_puts(out, "static FiberBlock *tur_fiber_block_new(void (*fn)(void), size_t stack_size) {\n");
    buf_puts(out, "    if (!stack_size) stack_size = 1024 * 1024;\n");
    buf_puts(out, "    FiberBlock *f = (FiberBlock *)calloc(1, sizeof(FiberBlock));\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"fiber: oom\\n\"); abort(); }\n");
    buf_puts(out, "    f->stack = (char *)malloc(stack_size);\n");
    buf_puts(out, "    if (!f->stack) { free(f); abort(); }\n");
    buf_puts(out, "    f->stack_size = stack_size; f->entry_fn = fn; f->done = 0;\n");
    /* SCH-004: Initialize migration_safe flag - fibers are migration-safe by default */
    buf_puts(out, "    f->migration_safe = true;\n");
    /* SCH-004: Copy global effect handler chain to fiber for migration safety */
    buf_puts(out, "    f->effect_handler_chain = (void *)global_effect_handler_chain;\n");
    buf_puts(out, "    getcontext(&f->ctx);\n");
    buf_puts(out, "    f->ctx.uc_stack.ss_sp = f->stack;\n");
    buf_puts(out, "    f->ctx.uc_stack.ss_size = stack_size;\n");
    buf_puts(out, "    f->ctx.uc_link = NULL;\n");
    buf_puts(out, "    uintptr_t _fp = (uintptr_t)f;\n");
    buf_puts(out, "    uint32_t _hi = (uint32_t)(_fp >> 32);\n");
    buf_puts(out, "    uint32_t _lo = (uint32_t)(_fp & 0xFFFFFFFFU);\n");
    buf_puts(out, "    makecontext(&f->ctx, (void(*)(void))tur_fiber_shim, 2, _hi, _lo);\n");
    buf_puts(out, "    return f;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int64_t tur_fiber_block_resume(FiberBlock *f, int64_t arg) {\n");
    buf_puts(out, "    if (!f || f->done) return f ? f->result : 0;\n");
    /* Phase T22: Check if fiber or its task group was cancelled before resuming */
    buf_puts(out, "    if (f->cancelled) { f->done = 1; return 0; }\n");
    buf_puts(out, "    if (f->task_group) {\n");
    buf_puts(out, "        typedef struct TaskGroupBlock { bool cancelled; } TaskGroupBlock;\n");
    buf_puts(out, "        if (((TaskGroupBlock *)f->task_group)->cancelled) { f->cancelled = 1; f->done = 1; return 0; }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    FiberBlock *_prev = tur_current_fiber;\n");
    buf_puts(out, "    tur_current_fiber = f;\n");
    buf_puts(out, "    f->arg = arg;\n");
    buf_puts(out, "    swapcontext(&f->caller_ctx, &f->ctx);\n");
    buf_puts(out, "    tur_current_fiber = _prev;\n");
    buf_puts(out, "    return f->result;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_block_yield(int64_t value) {\n");
    buf_puts(out, "    FiberBlock *f = tur_current_fiber;\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"fiber-yield: not in fiber\\n\"); abort(); }\n");
    buf_puts(out, "    f->result = value;\n");
    buf_puts(out, "    swapcontext(&f->ctx, &f->caller_ctx);\n");
    buf_puts(out, "}\n\n");
    /* Phase 19D: Effect-capture continuation helpers */
    buf_puts(out, "/* Phase 19D: Effect-capture continuation helpers */\n");
    buf_puts(out, "static int64_t tur_effect_cont_resume(int64_t k_as_int64, int64_t v) {\n");
    buf_puts(out, "    FiberBlock *fiber = (FiberBlock *)(intptr_t)k_as_int64;\n");
    buf_puts(out, "    TurEffectCaptureCtx *ctx = (TurEffectCaptureCtx *)fiber->eff_ctx;\n");
    buf_puts(out, "    if (!ctx) { fprintf(stderr, \"continuation error: not a capturable continuation\\n\"); abort(); }\n");
    buf_puts(out, "    return ctx->dispatch(ctx, k_as_int64, v);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static bool tur_effect_cont_valid(int64_t k_as_int64) {\n");
    buf_puts(out, "    FiberBlock *fiber = (FiberBlock *)(intptr_t)k_as_int64;\n");
    buf_puts(out, "    return !fiber->done;\n");
    buf_puts(out, "}\n\n");
    /* Phase T21: Fiber-local storage */
    buf_puts(out, "typedef struct FiberLocalEntry FiberLocalEntry;\n");
    buf_puts(out, "struct FiberLocalEntry {\n");
    buf_puts(out, "    int64_t key;\n");
    buf_puts(out, "    int64_t value;\n");
    buf_puts(out, "    FiberLocalEntry *next;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static void tur_fiber_local_free(FiberBlock *f) {\n");
    buf_puts(out, "    FiberLocalEntry *e = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    while (e) { FiberLocalEntry *n = e->next; free(e); e = n; }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int64_t tur_fiber_local_get(FiberBlock *f, int64_t key) {\n");
    buf_puts(out, "    FiberLocalEntry *e = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    while (e) { if (e->key == key) return e->value; e = e->next; }\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_local_set(FiberBlock *f, int64_t key, int64_t value) {\n");
    buf_puts(out, "    FiberLocalEntry *e = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    while (e) { if (e->key == key) { e->value = value; return; } e = e->next; }\n");
    buf_puts(out, "    FiberLocalEntry *n = (FiberLocalEntry *)malloc(sizeof(FiberLocalEntry));\n");
    buf_puts(out, "    if (!n) { fprintf(stderr, \"fiber-local: oom\\n\"); abort(); }\n");
    buf_puts(out, "    n->key = key; n->value = value;\n");
    buf_puts(out, "    n->next = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    f->fiber_local = (void *)n;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_block_free(FiberBlock *f) {\n");
    buf_puts(out, "    if (!f) return;\n");
    buf_puts(out, "    tur_fiber_local_free(f);\n");
    buf_puts(out, "    free(f->stack); free(f);\n");
    buf_puts(out, "}\n\n");
    /* Phase T22: Fiber cancellation */
    buf_puts(out, "static bool tur_fiber_cancelled(void) {\n");
    buf_puts(out, "    return tur_fiber_cancelled_flag;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_set_cancelled(bool c) {\n");
    buf_puts(out, "    tur_fiber_cancelled_flag = c;\n");
    buf_puts(out, "}\n\n");
    /* Phase T22: TaskGroup notification on fiber completion */
    buf_puts(out, "static void tur_task_group_notify_done(void *task_group) {\n");
    buf_puts(out, "    if (!task_group) return;\n");
    buf_puts(out, "    typedef struct TaskGroupBlock {\n");
    buf_puts(out, "        pthread_mutex_t lock;\n");
    buf_puts(out, "        pthread_cond_t done_cond;\n");
    buf_puts(out, "        int64_t task_count;\n");
    buf_puts(out, "        int64_t completed_count;\n");
    buf_puts(out, "        bool cancelled;\n");
    buf_puts(out, "        bool done;\n");
    buf_puts(out, "        pthread_t owner_thread;\n");
    buf_puts(out, "    } TaskGroupBlock;\n");
    buf_puts(out, "    TaskGroupBlock *g = (TaskGroupBlock *)task_group;\n");
    buf_puts(out, "    pthread_mutex_lock(&g->lock);\n");
    buf_puts(out, "    g->completed_count++;\n");
    buf_puts(out, "    if (g->completed_count >= g->task_count) {\n");
    buf_puts(out, "        g->done = true;\n");
    buf_puts(out, "        pthread_cond_broadcast(&g->done_cond);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    pthread_mutex_unlock(&g->lock);\n");
    buf_puts(out, "}\n\n");
    /* Phase T24: Forward declarations for timer wheel and IO waiters
     * (defined after scheduler, but referenced in scheduler run loops) */
    /* Phase T24: Timer wheel structs + globals (defined early so scheduler
     * run loops and helper functions can reference them) */
    buf_puts(out, "static int64_t tur_monotonic_ns(void) {\n");
    buf_puts(out, "    struct timespec ts;\n");
    buf_puts(out, "    clock_gettime(CLOCK_MONOTONIC, &ts);\n");
    buf_puts(out, "    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "typedef struct TurTimerEntry {\n");
    buf_puts(out, "    int64_t deadline_ns;\n");
    buf_puts(out, "    void (*callback)(void *);\n");
    buf_puts(out, "    void *arg;\n");
    buf_puts(out, "    int64_t id;\n");
    buf_puts(out, "    bool cancelled;\n");
    buf_puts(out, "} TurTimerEntry;\n\n");

    buf_puts(out, "typedef struct TurTimerWheel {\n");
    buf_puts(out, "    TurTimerEntry **heap;\n");
    buf_puts(out, "    int64_t len;\n");
    buf_puts(out, "    int64_t cap;\n");
    buf_puts(out, "    int64_t next_id;\n");
    buf_puts(out, "} TurTimerWheel;\n\n");

    buf_puts(out, "static TurTimerWheel *tur_global_timers = NULL;\n\n");

    buf_puts(out, "#define TUR_IO_READ  1\n");
    buf_puts(out, "#define TUR_IO_WRITE 2\n\n");

    buf_puts(out, "typedef struct TurIOWaiter {\n");
    buf_puts(out, "    int fd;\n");
    buf_puts(out, "    int events;\n");
    buf_puts(out, "    FiberBlock *fiber;\n");
    buf_puts(out, "    struct TurIOWaiter *next;\n");
    buf_puts(out, "} TurIOWaiter;\n\n");

    buf_puts(out, "static TurIOWaiter *tur_io_waiters = NULL;\n\n");

    /* Forward-declare timer/IO functions used in scheduler run loops */
    buf_puts(out, "static void tur_timer_wheel_tick(TurTimerWheel *w);\n");
    buf_puts(out, "static int64_t tur_timer_wheel_next_deadline_ns(TurTimerWheel *w);\n");
    buf_puts(out, "static int64_t tur_timer_wheel_insert(TurTimerWheel *w, int64_t deadline_ns, void (*cb)(void *), void *arg);\n");
    buf_puts(out, "static void tur_timer_wheel_cancel(TurTimerWheel *w, int64_t id);\n");
    buf_puts(out, "static TurTimerWheel *tur_timer_wheel_new(void);\n");
    buf_puts(out, "static void tur_io_register(int fd, int events, FiberBlock *fiber);\n");
    buf_puts(out, "static void tur_io_unregister(int fd);\n");
    buf_puts(out, "static void tur_io_poll(int64_t timeout_us);\n");
    buf_puts(out, "static void tur_scheduler_timeout(int64_t ms, void (*callback)(void *arg), void *arg);\n");
    buf_puts(out, "static void tur_tick_timers(void);\n");
    buf_puts(out, "static void tur_poll_io(int64_t timeout_us);\n");
    buf_puts(out, "static bool tur_has_pending_timers(void);\n");
    buf_puts(out, "static bool tur_has_pending_io(void);\n");
    buf_puts(out, "static int64_t tur_next_timer_wait_us(void);\n\n");

    /* Phase T21: Cooperative Scheduler for fibers */
    buf_puts(out, "typedef struct TurScheduler TurScheduler;\n");
    buf_puts(out, "struct TurScheduler {\n");
    buf_puts(out, "    FiberBlock **run_queue;\n");
    buf_puts(out, "    int64_t run_queue_cap;\n");
    buf_puts(out, "    int64_t run_queue_len;\n");
    buf_puts(out, "    int64_t run_queue_head;\n");
    buf_puts(out, "    int64_t run_queue_tail;\n");
    buf_puts(out, "    FiberBlock *current_fiber;\n");
    buf_puts(out, "    bool running;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static TurScheduler *tur_scheduler = NULL;\n\n");
    buf_puts(out, "static TurScheduler *tur_scheduler_new(void) {\n");
    buf_puts(out, "    TurScheduler *s = (TurScheduler *)calloc(1, sizeof(TurScheduler));\n");
    buf_puts(out, "    if (!s) { fprintf(stderr, \"scheduler: oom\\n\"); abort(); }\n");
    buf_puts(out, "    s->run_queue_cap = 64;\n");
    buf_puts(out, "    s->run_queue = (FiberBlock **)malloc(sizeof(FiberBlock *) * (size_t)s->run_queue_cap);\n");
    buf_puts(out, "    if (!s->run_queue) { free(s); fprintf(stderr, \"scheduler: queue oom\\n\"); abort(); }\n");
    buf_puts(out, "    s->run_queue_len = 0;\n");
    buf_puts(out, "    s->run_queue_head = 0;\n");
    buf_puts(out, "    s->run_queue_tail = 0;\n");
    buf_puts(out, "    s->current_fiber = NULL;\n");
    buf_puts(out, "    s->running = false;\n");
    buf_puts(out, "    return s;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static TurScheduler *tur_scheduler_current(void) {\n");
    buf_puts(out, "    return tur_scheduler;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_enqueue(TurScheduler *s, FiberBlock *f) {\n");
    buf_puts(out, "    if (s->run_queue_len >= s->run_queue_cap) {\n");
    buf_puts(out, "        int64_t new_cap = s->run_queue_cap * 2;\n");
    buf_puts(out, "        FiberBlock **new_q = (FiberBlock **)malloc(sizeof(FiberBlock *) * (size_t)new_cap);\n");
    buf_puts(out, "        if (!new_q) { fprintf(stderr, \"scheduler: grow oom\\n\"); abort(); }\n");
    buf_puts(out, "        for (int64_t i = 0; i < s->run_queue_len; i++)\n");
    buf_puts(out, "            new_q[i] = s->run_queue[(s->run_queue_head + i) % s->run_queue_cap];\n");
    buf_puts(out, "        free(s->run_queue);\n");
    buf_puts(out, "        s->run_queue = new_q;\n");
    buf_puts(out, "        s->run_queue_cap = new_cap;\n");
    buf_puts(out, "        s->run_queue_head = 0;\n");
    buf_puts(out, "        s->run_queue_tail = s->run_queue_len;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->run_queue[s->run_queue_tail] = f;\n");
    buf_puts(out, "    s->run_queue_tail = (s->run_queue_tail + 1) % s->run_queue_cap;\n");
    buf_puts(out, "    s->run_queue_len++;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static FiberBlock *tur_scheduler_dequeue(TurScheduler *s) {\n");
    buf_puts(out, "    if (s->run_queue_len == 0) return NULL;\n");
    buf_puts(out, "    FiberBlock *f = s->run_queue[s->run_queue_head];\n");
    buf_puts(out, "    s->run_queue_head = (s->run_queue_head + 1) % s->run_queue_cap;\n");
    buf_puts(out, "    s->run_queue_len--;\n");
    buf_puts(out, "    return f;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_spawn(TurScheduler *s, FiberBlock *f) {\n");
    buf_puts(out, "    tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_run(TurScheduler *s) {\n");
    buf_puts(out, "    s->running = true;\n");
    buf_puts(out, "    while (s->running) {\n");
    buf_puts(out, "        /* Phase T24: tick timers and poll IO before dequeuing */\n");
    buf_puts(out, "        tur_tick_timers();\n");
    buf_puts(out, "        tur_poll_io(0);\n");
    buf_puts(out, "        FiberBlock *f = tur_scheduler_dequeue(s);\n");
    buf_puts(out, "        if (!f) {\n");
    buf_puts(out, "            if (!tur_has_pending_timers() && !tur_has_pending_io()) break;\n");
    buf_puts(out, "            int64_t sleep_us = tur_next_timer_wait_us();\n");
    buf_puts(out, "            if (sleep_us < 0) sleep_us = 1000;\n");
    buf_puts(out, "            if (tur_has_pending_io()) tur_poll_io(sleep_us);\n");
    buf_puts(out, "            else { struct timespec ts = {0, sleep_us * 1000}; nanosleep(&ts, NULL); }\n");
    buf_puts(out, "            continue;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        s->current_fiber = f;\n");
    buf_puts(out, "        tur_fiber_block_resume(f, 0);\n");
    buf_puts(out, "        s->current_fiber = NULL;\n");
    buf_puts(out, "        if (!f->done && !f->parked) tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->running = false;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_run_to_completion(TurScheduler *s) {\n");
    buf_puts(out, "    s->running = true;\n");
    buf_puts(out, "    while (s->running) {\n");
    buf_puts(out, "        tur_tick_timers();\n");
    buf_puts(out, "        tur_poll_io(0);\n");
    buf_puts(out, "        FiberBlock *f = tur_scheduler_dequeue(s);\n");
    buf_puts(out, "        if (f) {\n");
    buf_puts(out, "            s->current_fiber = f;\n");
    buf_puts(out, "            tur_fiber_block_resume(f, 0);\n");
    buf_puts(out, "            s->current_fiber = NULL;\n");
    buf_puts(out, "            if (!f->done && !f->parked) tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            if (!tur_has_pending_timers() && !tur_has_pending_io()) break;\n");
    buf_puts(out, "            int64_t sleep_us = tur_next_timer_wait_us();\n");
    buf_puts(out, "            if (sleep_us < 0) sleep_us = 1000;\n");
    buf_puts(out, "            if (tur_has_pending_io()) tur_poll_io(sleep_us);\n");
    buf_puts(out, "            else { struct timespec ts = {0, sleep_us * 1000}; nanosleep(&ts, NULL); }\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->running = false;\n");
    buf_puts(out, "}\n\n");
    /* Phase T21: Scheduler yield/park/unpark */
    buf_puts(out, "static void tur_scheduler_yield(void) {\n");
    buf_puts(out, "    tur_fiber_block_yield(0);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_park(void) {\n");
    buf_puts(out, "    if (tur_current_fiber) tur_current_fiber->parked = 1;\n");
    buf_puts(out, "    tur_fiber_block_yield(0);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_unpark(FiberBlock *f) {\n");
    buf_puts(out, "    if (!f) return;\n");
    buf_puts(out, "    f->parked = 0;\n");
    buf_puts(out, "    if (tur_scheduler) tur_scheduler_enqueue(tur_scheduler, f);\n");
    buf_puts(out, "}\n\n");

    /* Phase T24: Timer wheel function implementations
     * (structs, globals, and tur_monotonic_ns defined earlier before scheduler) */
    buf_puts(out, "static TurTimerWheel *tur_timer_wheel_new(void) {\n");
    buf_puts(out, "    TurTimerWheel *w = (TurTimerWheel *)calloc(1, sizeof(TurTimerWheel));\n");
    buf_puts(out, "    if (!w) { fprintf(stderr, \"timer wheel: oom\\n\"); abort(); }\n");
    buf_puts(out, "    w->cap = 64;\n");
    buf_puts(out, "    w->heap = (TurTimerEntry **)malloc(sizeof(TurTimerEntry *) * (size_t)w->cap);\n");
    buf_puts(out, "    if (!w->heap) { free(w); fprintf(stderr, \"timer wheel: heap oom\\n\"); abort(); }\n");
    buf_puts(out, "    w->len = 0;\n");
    buf_puts(out, "    w->next_id = 1;\n");
    buf_puts(out, "    return w;\n");
    buf_puts(out, "}\n\n");

    /* Min-heap helpers: sift up and sift down by deadline */
    buf_puts(out, "static void tur_timer_heap_sift_up(TurTimerWheel *w, int64_t idx) {\n");
    buf_puts(out, "    while (idx > 0) {\n");
    buf_puts(out, "        int64_t parent = (idx - 1) / 2;\n");
    buf_puts(out, "        if (w->heap[parent]->deadline_ns <= w->heap[idx]->deadline_ns) break;\n");
    buf_puts(out, "        TurTimerEntry *tmp = w->heap[parent];\n");
    buf_puts(out, "        w->heap[parent] = w->heap[idx];\n");
    buf_puts(out, "        w->heap[idx] = tmp;\n");
    buf_puts(out, "        idx = parent;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_timer_heap_sift_down(TurTimerWheel *w, int64_t idx) {\n");
    buf_puts(out, "    while (1) {\n");
    buf_puts(out, "        int64_t smallest = idx;\n");
    buf_puts(out, "        int64_t left = 2 * idx + 1;\n");
    buf_puts(out, "        int64_t right = 2 * idx + 2;\n");
    buf_puts(out, "        if (left < w->len && w->heap[left]->deadline_ns < w->heap[smallest]->deadline_ns)\n");
    buf_puts(out, "            smallest = left;\n");
    buf_puts(out, "        if (right < w->len && w->heap[right]->deadline_ns < w->heap[smallest]->deadline_ns)\n");
    buf_puts(out, "            smallest = right;\n");
    buf_puts(out, "        if (smallest == idx) break;\n");
    buf_puts(out, "        TurTimerEntry *tmp = w->heap[smallest];\n");
    buf_puts(out, "        w->heap[smallest] = w->heap[idx];\n");
    buf_puts(out, "        w->heap[idx] = tmp;\n");
    buf_puts(out, "        idx = smallest;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static int64_t tur_timer_wheel_insert(TurTimerWheel *w, int64_t deadline_ns,\n");
    buf_puts(out, "                                       void (*cb)(void *), void *arg) {\n");
    buf_puts(out, "    if (w->len >= w->cap) {\n");
    buf_puts(out, "        int64_t new_cap = w->cap * 2;\n");
    buf_puts(out, "        TurTimerEntry **nh = (TurTimerEntry **)malloc(sizeof(TurTimerEntry *) * (size_t)new_cap);\n");
    buf_puts(out, "        if (!nh) { fprintf(stderr, \"timer wheel: grow oom\\n\"); abort(); }\n");
    buf_puts(out, "        for (int64_t i = 0; i < w->len; i++) nh[i] = w->heap[i];\n");
    buf_puts(out, "        free(w->heap);\n");
    buf_puts(out, "        w->heap = nh;\n");
    buf_puts(out, "        w->cap = new_cap;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    TurTimerEntry *e = (TurTimerEntry *)malloc(sizeof(TurTimerEntry));\n");
    buf_puts(out, "    if (!e) { fprintf(stderr, \"timer entry: oom\\n\"); abort(); }\n");
    buf_puts(out, "    e->deadline_ns = deadline_ns;\n");
    buf_puts(out, "    e->callback = cb;\n");
    buf_puts(out, "    e->arg = arg;\n");
    buf_puts(out, "    e->id = w->next_id++;\n");
    buf_puts(out, "    e->cancelled = false;\n");
    buf_puts(out, "    w->heap[w->len] = e;\n");
    buf_puts(out, "    tur_timer_heap_sift_up(w, w->len);\n");
    buf_puts(out, "    w->len++;\n");
    buf_puts(out, "    return e->id;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_timer_wheel_cancel(TurTimerWheel *w, int64_t id) {\n");
    buf_puts(out, "    for (int64_t i = 0; i < w->len; i++) {\n");
    buf_puts(out, "        if (w->heap[i]->id == id) { w->heap[i]->cancelled = true; return; }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_timer_wheel_tick(TurTimerWheel *w) {\n");
    buf_puts(out, "    int64_t now = tur_monotonic_ns();\n");
    buf_puts(out, "    while (w->len > 0 && w->heap[0]->deadline_ns <= now) {\n");
    buf_puts(out, "        TurTimerEntry *e = w->heap[0];\n");
    buf_puts(out, "        w->len--;\n");
    buf_puts(out, "        if (w->len > 0) {\n");
    buf_puts(out, "            w->heap[0] = w->heap[w->len];\n");
    buf_puts(out, "            tur_timer_heap_sift_down(w, 0);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        if (!e->cancelled) e->callback(e->arg);\n");
    buf_puts(out, "        free(e);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static int64_t tur_timer_wheel_next_deadline_ns(TurTimerWheel *w) {\n");
    buf_puts(out, "    while (w->len > 0 && w->heap[0]->cancelled) {\n");
    buf_puts(out, "        TurTimerEntry *e = w->heap[0];\n");
    buf_puts(out, "        w->len--;\n");
    buf_puts(out, "        if (w->len > 0) {\n");
    buf_puts(out, "            w->heap[0] = w->heap[w->len];\n");
    buf_puts(out, "            tur_timer_heap_sift_down(w, 0);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        free(e);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (w->len == 0) return -1;\n");
    buf_puts(out, "    return w->heap[0]->deadline_ns;\n");
    buf_puts(out, "}\n\n");

    /* tur_scheduler_timeout: same API, now uses timer wheel instead of thread-per-timeout */
    buf_puts(out, "static void tur_scheduler_timeout(int64_t ms, void (*callback)(void *arg), void *arg) {\n");
    buf_puts(out, "    if (!tur_global_timers) tur_global_timers = tur_timer_wheel_new();\n");
    buf_puts(out, "    int64_t deadline = tur_monotonic_ns() + ms * 1000000LL;\n");
    buf_puts(out, "    tur_timer_wheel_insert(tur_global_timers, deadline, callback, arg);\n");
    buf_puts(out, "}\n\n");

    /* Phase T24: IO waiter function implementations
     * (struct, defines, and global defined earlier before scheduler) */
    buf_puts(out, "static void tur_io_register(int fd, int events, FiberBlock *fiber) {\n");
    buf_puts(out, "    TurIOWaiter *w = (TurIOWaiter *)malloc(sizeof(TurIOWaiter));\n");
    buf_puts(out, "    if (!w) { fprintf(stderr, \"io waiter: oom\\n\"); abort(); }\n");
    buf_puts(out, "    w->fd = fd;\n");
    buf_puts(out, "    w->events = events;\n");
    buf_puts(out, "    w->fiber = fiber;\n");
    buf_puts(out, "    w->next = tur_io_waiters;\n");
    buf_puts(out, "    tur_io_waiters = w;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_io_unregister(int fd) {\n");
    buf_puts(out, "    TurIOWaiter **pp = &tur_io_waiters;\n");
    buf_puts(out, "    while (*pp) {\n");
    buf_puts(out, "        if ((*pp)->fd == fd) {\n");
    buf_puts(out, "            TurIOWaiter *tmp = *pp;\n");
    buf_puts(out, "            *pp = tmp->next;\n");
    buf_puts(out, "            free(tmp);\n");
    buf_puts(out, "            return;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        pp = &(*pp)->next;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_io_poll(int64_t timeout_us) {\n");
    buf_puts(out, "    if (!tur_io_waiters) return;\n");
    buf_puts(out, "    fd_set rfds, wfds;\n");
    buf_puts(out, "    FD_ZERO(&rfds);\n");
    buf_puts(out, "    FD_ZERO(&wfds);\n");
    buf_puts(out, "    int maxfd = -1;\n");
    buf_puts(out, "    for (TurIOWaiter *w = tur_io_waiters; w; w = w->next) {\n");
    buf_puts(out, "        if (w->events & TUR_IO_READ)  FD_SET(w->fd, &rfds);\n");
    buf_puts(out, "        if (w->events & TUR_IO_WRITE) FD_SET(w->fd, &wfds);\n");
    buf_puts(out, "        if (w->fd > maxfd) maxfd = w->fd;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    struct timeval tv;\n");
    buf_puts(out, "    tv.tv_sec = (long)(timeout_us / 1000000);\n");
    buf_puts(out, "    tv.tv_usec = (long)(timeout_us % 1000000);\n");
    buf_puts(out, "    int ret = select(maxfd + 1, &rfds, &wfds, NULL, &tv);\n");
    buf_puts(out, "    if (ret <= 0) return;\n");
    buf_puts(out, "    TurIOWaiter **pp = &tur_io_waiters;\n");
    buf_puts(out, "    while (*pp) {\n");
    buf_puts(out, "        TurIOWaiter *w = *pp;\n");
    buf_puts(out, "        bool ready = false;\n");
    buf_puts(out, "        if ((w->events & TUR_IO_READ)  && FD_ISSET(w->fd, &rfds))  ready = true;\n");
    buf_puts(out, "        if ((w->events & TUR_IO_WRITE) && FD_ISSET(w->fd, &wfds)) ready = true;\n");
    buf_puts(out, "        if (ready) {\n");
    buf_puts(out, "            FiberBlock *f = w->fiber;\n");
    buf_puts(out, "            *pp = w->next;\n");
    buf_puts(out, "            free(w);\n");
    buf_puts(out, "            tur_scheduler_unpark(f);\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            pp = &w->next;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    /* Phase T24: Helper functions for scheduler integration (implementations
     * of the forward-declared functions used in the scheduler run loops) */
    buf_puts(out, "static void tur_tick_timers(void) {\n");
    buf_puts(out, "    if (tur_global_timers) tur_timer_wheel_tick(tur_global_timers);\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_poll_io(int64_t timeout_us) {\n");
    buf_puts(out, "    if (tur_io_waiters) tur_io_poll(timeout_us);\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static bool tur_has_pending_timers(void) {\n");
    buf_puts(out, "    return tur_global_timers && tur_global_timers->len > 0;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static bool tur_has_pending_io(void) {\n");
    buf_puts(out, "    return tur_io_waiters != NULL;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static int64_t tur_next_timer_wait_us(void) {\n");
    buf_puts(out, "    if (!tur_global_timers || tur_global_timers->len == 0) return -1;\n");
    buf_puts(out, "    int64_t next = tur_timer_wheel_next_deadline_ns(tur_global_timers);\n");
    buf_puts(out, "    if (next < 0) return -1;\n");
    buf_puts(out, "    int64_t diff = (next - tur_monotonic_ns()) / 1000;\n");
    buf_puts(out, "    return diff > 0 ? diff : 0;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "#ifdef __clang__\n");
    buf_puts(out, "#pragma clang diagnostic pop\n");
    buf_puts(out, "#endif\n\n");
    /* Phase T21: Scheduler run_one helper */
    buf_puts(out, "static void tur_scheduler_run_one(TurScheduler *s) {\n");
    buf_puts(out, "    FiberBlock *f = tur_scheduler_dequeue(s);\n");
    buf_puts(out, "    if (!f) { return; }\n");
    buf_puts(out, "    s->current_fiber = f;\n");
    buf_puts(out, "    tur_fiber_block_resume(f, 0);\n");
    buf_puts(out, "    s->current_fiber = NULL;\n");
    buf_puts(out, "    if (!f->done && !f->parked) tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "}\n\n");

    /* Phase T23: Minimal multi-threaded scheduler runtime.
     * Provides the API surface used by stdlib/scheduler_mt.tur.  This is a
     * simple shared-queue implementation (not work-stealing); sufficient to
     * run cooperative fibers across N OS threads for fixtures that just need
     * to observe distinct thread ids. */
    buf_puts(out, "/* Phase T23: Multi-threaded scheduler */\n");
    buf_puts(out, "typedef struct TurSchedulerMT {\n");
    buf_puts(out, "    pthread_t       *threads;\n");
    buf_puts(out, "    size_t           n_threads;\n");
    buf_puts(out, "    pthread_mutex_t  lock;\n");
    buf_puts(out, "    pthread_cond_t   cond;\n");
    buf_puts(out, "    FiberBlock     **queue;\n");
    buf_puts(out, "    size_t           qhead;\n");
    buf_puts(out, "    size_t           qtail;\n");
    buf_puts(out, "    size_t           qcap;\n");
    buf_puts(out, "    bool             running;\n");
    buf_puts(out, "    int              active;\n");
    buf_puts(out, "} TurSchedulerMT;\n\n");
    buf_puts(out, "static __thread TurSchedulerMT *tur_current_scheduler_mt = NULL;\n\n");
    buf_puts(out, "static void tur_scheduler_mt_enqueue_locked(TurSchedulerMT *s, FiberBlock *f) {\n");
    buf_puts(out, "    if (((s->qtail + 1) % s->qcap) == s->qhead) {\n");
    buf_puts(out, "        size_t ncap = s->qcap * 2;\n");
    buf_puts(out, "        FiberBlock **nq = (FiberBlock **)calloc(ncap, sizeof(FiberBlock *));\n");
    buf_puts(out, "        size_t k = 0;\n");
    buf_puts(out, "        for (size_t i = s->qhead; i != s->qtail; i = (i + 1) % s->qcap) nq[k++] = s->queue[i];\n");
    buf_puts(out, "        free(s->queue);\n");
    buf_puts(out, "        s->queue = nq; s->qcap = ncap; s->qhead = 0; s->qtail = k;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->queue[s->qtail] = f;\n");
    buf_puts(out, "    s->qtail = (s->qtail + 1) % s->qcap;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void *tur_scheduler_mt_worker(void *arg) {\n");
    buf_puts(out, "    TurSchedulerMT *s = (TurSchedulerMT *)arg;\n");
    buf_puts(out, "    tur_current_scheduler_mt = s;\n");
    buf_puts(out, "    for (;;) {\n");
    buf_puts(out, "        pthread_mutex_lock(&s->lock);\n");
    buf_puts(out, "        while (s->running && s->qhead == s->qtail) {\n");
    buf_puts(out, "            pthread_cond_wait(&s->cond, &s->lock);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        if (!s->running && s->qhead == s->qtail) {\n");
    buf_puts(out, "            pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "            break;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        FiberBlock *f = s->queue[s->qhead];\n");
    buf_puts(out, "        s->qhead = (s->qhead + 1) % s->qcap;\n");
    buf_puts(out, "        s->active++;\n");
    buf_puts(out, "        pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "        tur_fiber_block_resume(f, 0);\n");
    buf_puts(out, "        pthread_mutex_lock(&s->lock);\n");
    buf_puts(out, "        s->active--;\n");
    buf_puts(out, "        if (!f->done && !f->parked) tur_scheduler_mt_enqueue_locked(s, f);\n");
    buf_puts(out, "        pthread_cond_broadcast(&s->cond);\n");
    buf_puts(out, "        pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return NULL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static TurSchedulerMT *tur_scheduler_mt_new(size_t n_threads) {\n");
    buf_puts(out, "    if (n_threads == 0) n_threads = 1;\n");
    buf_puts(out, "    TurSchedulerMT *s = (TurSchedulerMT *)calloc(1, sizeof(TurSchedulerMT));\n");
    buf_puts(out, "    if (!s) return NULL;\n");
    buf_puts(out, "    pthread_mutex_init(&s->lock, NULL);\n");
    buf_puts(out, "    pthread_cond_init(&s->cond, NULL);\n");
    buf_puts(out, "    s->qcap = 32;\n");
    buf_puts(out, "    s->queue = (FiberBlock **)calloc(s->qcap, sizeof(FiberBlock *));\n");
    buf_puts(out, "    s->n_threads = n_threads;\n");
    buf_puts(out, "    s->threads = (pthread_t *)calloc(n_threads, sizeof(pthread_t));\n");
    buf_puts(out, "    s->running = true;\n");
    buf_puts(out, "    for (size_t i = 0; i < n_threads; i++) {\n");
    buf_puts(out, "        pthread_create(&s->threads[i], NULL, tur_scheduler_mt_worker, s);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return s;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_free(TurSchedulerMT *s) {\n");
    buf_puts(out, "    if (!s) return;\n");
    buf_puts(out, "    pthread_mutex_lock(&s->lock);\n");
    buf_puts(out, "    /* Drain: wait until queue is empty and no fiber is active */\n");
    buf_puts(out, "    while (s->qhead != s->qtail || s->active > 0) {\n");
    buf_puts(out, "        pthread_cond_wait(&s->cond, &s->lock);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->running = false;\n");
    buf_puts(out, "    pthread_cond_broadcast(&s->cond);\n");
    buf_puts(out, "    pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "    for (size_t i = 0; i < s->n_threads; i++) pthread_join(s->threads[i], NULL);\n");
    buf_puts(out, "    pthread_mutex_destroy(&s->lock);\n");
    buf_puts(out, "    pthread_cond_destroy(&s->cond);\n");
    buf_puts(out, "    free(s->queue); free(s->threads); free(s);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_spawn(TurSchedulerMT *s, FiberBlock *f) {\n");
    buf_puts(out, "    if (!s || !f) return;\n");
    buf_puts(out, "    pthread_mutex_lock(&s->lock);\n");
    buf_puts(out, "    tur_scheduler_mt_enqueue_locked(s, f);\n");
    buf_puts(out, "    pthread_cond_signal(&s->cond);\n");
    buf_puts(out, "    pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_run(TurSchedulerMT *s) {\n");
    buf_puts(out, "    /* Worker threads run automatically; this is a no-op for the\n");
    buf_puts(out, "     * shared-queue impl.  Kept for API compatibility. */\n");
    buf_puts(out, "    (void)s;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int64_t tur_scheduler_mt_thread_id(void) {\n");
    buf_puts(out, "    return (int64_t)(uintptr_t)pthread_self();\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_set_current(TurSchedulerMT *s) {\n");
    buf_puts(out, "    tur_current_scheduler_mt = s;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static TurSchedulerMT *tur_scheduler_mt_current(void) {\n");
    buf_puts(out, "    return tur_current_scheduler_mt;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_yield(void) {\n");
    buf_puts(out, "    /* Cooperative yield: re-enqueues current fiber via the worker loop. */\n");
    buf_puts(out, "    tur_fiber_block_yield(0);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_park(void) {\n");
    buf_puts(out, "    FiberBlock *f = tur_current_fiber;\n");
    buf_puts(out, "    if (f) f->parked = 1;\n");
    buf_puts(out, "    tur_fiber_block_yield(0);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_unpark(FiberBlock *f) {\n");
    buf_puts(out, "    TurSchedulerMT *s = tur_current_scheduler_mt;\n");
    buf_puts(out, "    if (!f || !s) return;\n");
    buf_puts(out, "    f->parked = 0;\n");
    buf_puts(out, "    pthread_mutex_lock(&s->lock);\n");
    buf_puts(out, "    tur_scheduler_mt_enqueue_locked(s, f);\n");
    buf_puts(out, "    pthread_cond_signal(&s->cond);\n");
    buf_puts(out, "    pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "}\n\n");

    /* Phase T21-F: async/await runtime - fiber-based Futures */
    buf_puts(out, "/* Phase T21-F: async/await runtime - fiber-based Futures */\n");
    buf_puts(out, "typedef enum { FUTURE_PENDING, FUTURE_FULFILLED, FUTURE_REJECTED } TurFutureStatus;\n\n");
    
    buf_puts(out, "typedef struct TurFuture TurFuture;\n");
    buf_puts(out, "struct TurFuture {\n");
    buf_puts(out, "    TurFutureStatus status;\n");
    buf_puts(out, "    int64_t value;\n");
    buf_puts(out, "    const char *error;  /* NULL if no error */\n");
    buf_puts(out, "    FiberBlock *fiber;  /* The fiber running the async task */\n");
    buf_puts(out, "    struct { void (*fn)(TurFuture *, int64_t); void *env; } on_complete;\n");
    buf_puts(out, "};\n\n");
    
    buf_puts(out, "/* Create a new pending future */\n");
    buf_puts(out, "static TurFuture *tur_future_new(void) {\n");
    buf_puts(out, "    TurFuture *f = (TurFuture *)calloc(1, sizeof(TurFuture));\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"future: oom\\n\"); abort(); }\n");
    buf_puts(out, "    f->status = FUTURE_PENDING;\n");
    buf_puts(out, "    f->fiber = NULL;\n");
    buf_puts(out, "    f->on_complete.fn = NULL;\n");
    buf_puts(out, "    return f;\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Fulfill a future with a value */\n");
    buf_puts(out, "static void tur_future_fulfill(TurFuture *f, int64_t value) {\n");
    buf_puts(out, "    if (!f || f->status != FUTURE_PENDING) return;\n");
    buf_puts(out, "    f->status = FUTURE_FULFILLED;\n");
    buf_puts(out, "    f->value = value;\n");
    buf_puts(out, "    if (f->on_complete.fn) f->on_complete.fn(f, value);\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Reject a future with an error message */\n");
    buf_puts(out, "static void tur_future_reject(TurFuture *f, const char *error) {\n");
    buf_puts(out, "    if (!f || f->status != FUTURE_PENDING) return;\n");
    buf_puts(out, "    f->status = FUTURE_REJECTED;\n");
    buf_puts(out, "    f->error = error;\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Check if future is done (fulfilled or rejected) */\n");
    buf_puts(out, "static int tur_future_done(TurFuture *f) {\n");
    buf_puts(out, "    return f && (f->status == FUTURE_FULFILLED || f->status == FUTURE_REJECTED);\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Get future value (only valid if fulfilled) */\n");
    buf_puts(out, "static int64_t tur_future_get(TurFuture *f) {\n");
    buf_puts(out, "    if (!f || f->status != FUTURE_FULFILLED) return 0;\n");
    buf_puts(out, "    return f->value;\n");
    buf_puts(out, "}\n\n");
    
    /* Create a future that runs fn() and fulfills it with the result */
    buf_puts(out, "static TurFuture *tur_async_fiber(int64_t (*fn)(void)) {\n");
    buf_puts(out, "    TurFuture *future = tur_future_new();\n");
    buf_puts(out, "    if (!tur_scheduler) {\n");
    buf_puts(out, "        /* AW-005: Initialize scheduler on first use */\n");
    buf_puts(out, "        tur_scheduler = tur_scheduler_new();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* AW-005: Simplified v1 - call function directly and fulfill */\n");
    buf_puts(out, "    int64_t result = fn();\n");
    buf_puts(out, "    tur_future_fulfill(future, result);\n");
    buf_puts(out, "    return future;\n");
    buf_puts(out, "}\n\n");
    
    /* Await a future using shift + scheduler. If future is done, return value directly. */
    buf_puts(out, "/* AW-004: await lowering with shift + scheduler callback */\n");
    buf_puts(out, "static int64_t tur_await_future(TurFuture *f) {\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"await: null future\\n\"); abort(); }\n");
    buf_puts(out, "    if (tur_future_done(f)) {\n");
    buf_puts(out, "        if (f->status == FUTURE_REJECTED) {\n");
    buf_puts(out, "            fprintf(stderr, \"await: future rejected: %s\\n\", f->error ? f->error : \"unknown\");\n");
    buf_puts(out, "            abort();\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return f->value;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Future not ready */\n");
    buf_puts(out, "    if (!tur_current_fiber) {\n");
    buf_puts(out, "        /* Not in a fiber - run the scheduler until future is done */\n");
    buf_puts(out, "        if (!tur_scheduler) {\n");
    buf_puts(out, "            fprintf(stderr, \"await: no scheduler and not in fiber\\n\");\n");
    buf_puts(out, "            abort();\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        while (!tur_future_done(f)) {\n");
    buf_puts(out, "            tur_scheduler_run_one(tur_scheduler);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        /* In a fiber - yield and let scheduler resume us */\n");
    buf_puts(out, "        /* Register a callback to re-enqueue this fiber when future completes */\n");
    buf_puts(out, "        f->on_complete.fn = (void (*)(TurFuture *, int64_t))tur_fiber_block_resume;\n");
    buf_puts(out, "        f->on_complete.env = (void *)tur_current_fiber;\n");
    buf_puts(out, "        tur_fiber_block_yield(0);\n");
    buf_puts(out, "        /* When we resume, the future should be done */\n");
    buf_puts(out, "        if (tur_future_done(f) && f->status == FUTURE_REJECTED) {\n");
    buf_puts(out, "            fprintf(stderr, \"await: future rejected: %s\\n\", f->error ? f->error : \"unknown\");\n");
    buf_puts(out, "            abort();\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return f->value;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (f->status == FUTURE_REJECTED) {\n");
    buf_puts(out, "        fprintf(stderr, \"await: future rejected: %s\\n\", f->error ? f->error : \"unknown\");\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return f->value;\n");
    buf_puts(out, "}\n\n");
    
    /* Free a future */
    buf_puts(out, "static void tur_future_free(TurFuture *f) {\n");
    buf_puts(out, "    if (!f) return;\n");
    buf_puts(out, "    free(f);\n");
    buf_puts(out, "}\n\n");
    
    /* Keep old TurAsyncTask for backward compatibility */
    buf_puts(out, "/* Backward compatibility: TurAsyncTask = TurFuture */\n");
    buf_puts(out, "typedef TurFuture TurAsyncTask;\n\n");

    /* Phase SEL0: select waiter infrastructure for fair multi-channel blocking */
    buf_puts(out, "/* Phase SEL0: TurSelectWaiter -- select waiter for fair multi-channel blocking */\n");
    buf_puts(out, "typedef struct TurSelectWaiter TurSelectWaiter;\n");
    buf_puts(out, "struct TurSelectWaiter {\n");
    buf_puts(out, "    pthread_mutex_t *wakeup_mutex;\n");
    buf_puts(out, "    pthread_cond_t  *wakeup_cond;\n");
    buf_puts(out, "    volatile int    *selected_idx;\n");
    buf_puts(out, "    int              clause_idx;\n");
    buf_puts(out, "    TurSelectWaiter *next;\n");
    buf_puts(out, "};\n\n");

    buf_puts(out, "/* Phase SEL0: signal the first unselected waiter in the list */\n");
    buf_puts(out, "static void tur_waiter_signal_one(void *waiter_list) {\n");
    buf_puts(out, "    TurSelectWaiter *w = (TurSelectWaiter *)waiter_list;\n");
    buf_puts(out, "    while (w) {\n");
    buf_puts(out, "        int exp = -1;\n");
    buf_puts(out, "        if (__atomic_compare_exchange_n(w->selected_idx, &exp, w->clause_idx, 0,\n");
    buf_puts(out, "                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {\n");
    buf_puts(out, "            pthread_mutex_lock(w->wakeup_mutex);\n");
    buf_puts(out, "            pthread_cond_signal(w->wakeup_cond);\n");
    buf_puts(out, "            pthread_mutex_unlock(w->wakeup_mutex);\n");
    buf_puts(out, "            return;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        w = w->next;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    /* Phase SEL1: tur_select_blocking -- fair multi-channel blocking select */
    buf_puts(out, "/* Phase SEL1: TurSelectClause -- clause descriptor for tur_select_blocking */\n");
    buf_puts(out, "typedef struct { void *chan; int op; int64_t val; } TurSelectClause;\n\n");
    /* Persistent xorshift32 state: static so it survives across calls (including
     * tail-call-optimised loops) and accumulates entropy across the program. */
    buf_puts(out, "/* Phase SEL1: persistent xorshift32 PRNG for fair select */\n");
    buf_puts(out, "static volatile uint32_t __tur_xr_state = 1u;\n\n");

    buf_puts(out, "/* Phase SEL1: tur_select_blocking -- block on all clauses; wake on first ready.\n");
    buf_puts(out, " * clauses: array of TurSelectClause (op 0=recv, 1=send)\n");
    buf_puts(out, " * n: number of clauses\n");
    buf_puts(out, " * has_default: 1 if a :default arm is present\n");
    buf_puts(out, " * Returns: index of the clause that fired, or -1 for default */\n");
    buf_puts(out, "static int tur_select_blocking(TurSelectClause *clauses, int n, int has_default) {\n");
    buf_puts(out, "    typedef struct {\n");
    buf_puts(out, "        pthread_mutex_t lock; pthread_cond_t not_full; pthread_cond_t not_empty;\n");
    buf_puts(out, "        int64_t *buf; int64_t head; int64_t tail; int64_t count; int64_t cap;\n");
    buf_puts(out, "        void *recv_waiters; void *send_waiters;\n");
    buf_puts(out, "    } ChanBlock_;\n");
    buf_puts(out, "    if (n <= 0) return -2;\n");
    /* Phase 1: non-blocking scan */
    buf_puts(out, "    /* Phase 1: non-blocking scan -- try all clauses without blocking */\n");
    buf_puts(out, "    /* Collect unique channels for lock ordering */\n");
    buf_puts(out, "    ChanBlock_ *lock_order[64];\n");
    buf_puts(out, "    int n_unique = 0;\n");
    buf_puts(out, "    for (int i = 0; i < n && n_unique < 64; i++) {\n");
    buf_puts(out, "        ChanBlock_ *ch = (ChanBlock_ *)clauses[i].chan;\n");
    buf_puts(out, "        int found = 0;\n");
    buf_puts(out, "        for (int j = 0; j < n_unique; j++) if (lock_order[j] == ch) { found = 1; break; }\n");
    buf_puts(out, "        if (!found) lock_order[n_unique++] = ch;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Sort channels by address for deadlock-free locking order */\n");
    buf_puts(out, "    for (int i = 0; i < n_unique - 1; i++)\n");
    buf_puts(out, "        for (int j = i + 1; j < n_unique; j++)\n");
    buf_puts(out, "            if ((uintptr_t)lock_order[i] > (uintptr_t)lock_order[j]) {\n");
    buf_puts(out, "                ChanBlock_ *tmp = lock_order[i]; lock_order[i] = lock_order[j]; lock_order[j] = tmp;\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "    /* Acquire all locks */\n");
    buf_puts(out, "    for (int i = 0; i < n_unique; i++) pthread_mutex_lock(&lock_order[i]->lock);\n");
    buf_puts(out, "    /* Final non-blocking scan under all locks */\n");
    buf_puts(out, "    int ready[64]; int n_ready = 0;\n");
    buf_puts(out, "    for (int i = 0; i < n; i++) {\n");
    buf_puts(out, "        ChanBlock_ *ch = (ChanBlock_ *)clauses[i].chan;\n");
    buf_puts(out, "        if (clauses[i].op == 0) { if (ch->count > 0) ready[n_ready++] = i; }\n");
    buf_puts(out, "        else                   { if (ch->count < ch->cap) ready[n_ready++] = i; }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (n_ready > 0) {\n");
    buf_puts(out, "        /* Fair: pick uniformly at random among ready clauses.\n");
    buf_puts(out, "         * Use a persistent xorshift32 state so repeated calls from the\n");
    buf_puts(out, "         * same site (e.g. a TCO loop) don't always produce the same winner. */\n");
    buf_puts(out, "        uint32_t xr = __tur_xr_state;\n");
    buf_puts(out, "        xr ^= (uint32_t)(uintptr_t)clauses;\n");
    buf_puts(out, "        if (!xr) xr = 0x9e3779b9u;\n");
    buf_puts(out, "        xr ^= xr << 13; xr ^= xr >> 17; xr ^= xr << 5;\n");
    buf_puts(out, "        __tur_xr_state = xr;\n");
    buf_puts(out, "        int winner = ready[xr % (uint32_t)n_ready];\n");
    buf_puts(out, "        ChanBlock_ *wch = (ChanBlock_ *)clauses[winner].chan;\n");
    buf_puts(out, "        if (clauses[winner].op == 0) { /* recv */\n");
    buf_puts(out, "            clauses[winner].val = wch->buf[wch->head];\n");
    buf_puts(out, "            wch->head = (wch->head + 1) % wch->cap; wch->count--;\n");
    buf_puts(out, "            pthread_cond_signal(&wch->not_full);\n");
    buf_puts(out, "            tur_waiter_signal_one(wch->send_waiters);\n");
    buf_puts(out, "        } else { /* send */\n");
    buf_puts(out, "            wch->buf[wch->tail] = clauses[winner].val;\n");
    buf_puts(out, "            wch->tail = (wch->tail + 1) % wch->cap; wch->count++;\n");
    buf_puts(out, "            pthread_cond_signal(&wch->not_empty);\n");
    buf_puts(out, "            tur_waiter_signal_one(wch->recv_waiters);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        for (int i = 0; i < n_unique; i++) pthread_mutex_unlock(&lock_order[i]->lock);\n");
    buf_puts(out, "        return winner;\n");
    buf_puts(out, "    }\n");
    /* Phase 2: default arm */
    buf_puts(out, "    /* Phase 2: default arm */\n");
    buf_puts(out, "    if (has_default) {\n");
    buf_puts(out, "        for (int i = 0; i < n_unique; i++) pthread_mutex_unlock(&lock_order[i]->lock);\n");
    buf_puts(out, "        return -1;\n");
    buf_puts(out, "    }\n");
    /* Phase 3: register waiters and sleep */
    buf_puts(out, "    /* Phase 3: register waiters and sleep until one fires */\n");
    buf_puts(out, "    pthread_mutex_t wakeup_mutex;\n");
    buf_puts(out, "    pthread_cond_t  wakeup_cond;\n");
    buf_puts(out, "    pthread_mutex_init(&wakeup_mutex, NULL);\n");
    buf_puts(out, "    pthread_cond_init(&wakeup_cond, NULL);\n");
    buf_puts(out, "    volatile int selected_idx = -1;\n");
    buf_puts(out, "    TurSelectWaiter waiters[64];\n");
    buf_puts(out, "    int wn = n < 64 ? n : 64;\n");
    buf_puts(out, "    for (int i = 0; i < wn; i++) {\n");
    buf_puts(out, "        waiters[i].wakeup_mutex = &wakeup_mutex;\n");
    buf_puts(out, "        waiters[i].wakeup_cond  = &wakeup_cond;\n");
    buf_puts(out, "        waiters[i].selected_idx = &selected_idx;\n");
    buf_puts(out, "        waiters[i].clause_idx   = i;\n");
    buf_puts(out, "        ChanBlock_ *ch = (ChanBlock_ *)clauses[i].chan;\n");
    buf_puts(out, "        if (clauses[i].op == 0) { /* recv waiter */\n");
    buf_puts(out, "            waiters[i].next = (TurSelectWaiter *)ch->recv_waiters;\n");
    buf_puts(out, "            ch->recv_waiters = &waiters[i];\n");
    buf_puts(out, "        } else { /* send waiter */\n");
    buf_puts(out, "            waiters[i].next = (TurSelectWaiter *)ch->send_waiters;\n");
    buf_puts(out, "            ch->send_waiters = &waiters[i];\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Release all channel locks */\n");
    buf_puts(out, "    for (int i = 0; i < n_unique; i++) pthread_mutex_unlock(&lock_order[i]->lock);\n");
    buf_puts(out, "    /* Sleep until woken by a channel operation or cancelled (TC1) */\n");
    buf_puts(out, "    pthread_mutex_lock(&wakeup_mutex);\n");
    buf_puts(out, "    while (selected_idx == -1) {\n");
    buf_puts(out, "        if (tur_thread_cancel_requested()) {\n");
    buf_puts(out, "            pthread_mutex_unlock(&wakeup_mutex);\n");
    buf_puts(out, "            /* Deregister all waiters before cancelling */\n");
    buf_puts(out, "            for (int __ci = 0; __ci < wn; __ci++) {\n");
    buf_puts(out, "                ChanBlock_ *__cch = (ChanBlock_ *)clauses[__ci].chan;\n");
    buf_puts(out, "                pthread_mutex_lock(&__cch->lock);\n");
    buf_puts(out, "                if (clauses[__ci].op == 0) {\n");
    buf_puts(out, "                    TurSelectWaiter **pp = (TurSelectWaiter **)&__cch->recv_waiters;\n");
    buf_puts(out, "                    while (*pp && *pp != &waiters[__ci]) pp = &(*pp)->next;\n");
    buf_puts(out, "                    if (*pp) *pp = (*pp)->next;\n");
    buf_puts(out, "                } else {\n");
    buf_puts(out, "                    TurSelectWaiter **pp = (TurSelectWaiter **)&__cch->send_waiters;\n");
    buf_puts(out, "                    while (*pp && *pp != &waiters[__ci]) pp = &(*pp)->next;\n");
    buf_puts(out, "                    if (*pp) *pp = (*pp)->next;\n");
    buf_puts(out, "                }\n");
    buf_puts(out, "                pthread_mutex_unlock(&__cch->lock);\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            pthread_mutex_destroy(&wakeup_mutex);\n");
    buf_puts(out, "            pthread_cond_destroy(&wakeup_cond);\n");
    buf_puts(out, "            tur_thread_do_cancel();\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        struct timespec __sel_ts;\n");
    buf_puts(out, "        clock_gettime(CLOCK_REALTIME, &__sel_ts);\n");
    buf_puts(out, "        long __sel_ns = __sel_ts.tv_nsec + 5000000L;\n");
    buf_puts(out, "        __sel_ts.tv_sec += __sel_ns / 1000000000L;\n");
    buf_puts(out, "        __sel_ts.tv_nsec = __sel_ns % 1000000000L;\n");
    buf_puts(out, "        pthread_cond_timedwait(&wakeup_cond, &wakeup_mutex, &__sel_ts);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    int winner = selected_idx;\n");
    buf_puts(out, "    pthread_mutex_unlock(&wakeup_mutex);\n");
    buf_puts(out, "    /* Deregister all waiters */\n");
    buf_puts(out, "    for (int i = 0; i < wn; i++) {\n");
    buf_puts(out, "        ChanBlock_ *ch = (ChanBlock_ *)clauses[i].chan;\n");
    buf_puts(out, "        pthread_mutex_lock(&ch->lock);\n");
    buf_puts(out, "        if (clauses[i].op == 0) {\n");
    buf_puts(out, "            TurSelectWaiter **pp = (TurSelectWaiter **)&ch->recv_waiters;\n");
    buf_puts(out, "            while (*pp && *pp != &waiters[i]) pp = &(*pp)->next;\n");
    buf_puts(out, "            if (*pp) *pp = (*pp)->next;\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            TurSelectWaiter **pp = (TurSelectWaiter **)&ch->send_waiters;\n");
    buf_puts(out, "            while (*pp && *pp != &waiters[i]) pp = &(*pp)->next;\n");
    buf_puts(out, "            if (*pp) *pp = (*pp)->next;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        pthread_mutex_unlock(&ch->lock);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Execute the winning clause */\n");
    buf_puts(out, "    ChanBlock_ *wch = (ChanBlock_ *)clauses[winner].chan;\n");
    buf_puts(out, "    pthread_mutex_lock(&wch->lock);\n");
    buf_puts(out, "    if (clauses[winner].op == 0) { /* recv */\n");
    buf_puts(out, "        while (wch->count == 0) pthread_cond_wait(&wch->not_empty, &wch->lock);\n");
    buf_puts(out, "        clauses[winner].val = wch->buf[wch->head];\n");
    buf_puts(out, "        wch->head = (wch->head + 1) % wch->cap; wch->count--;\n");
    buf_puts(out, "        pthread_cond_signal(&wch->not_full);\n");
    buf_puts(out, "        tur_waiter_signal_one(wch->send_waiters);\n");
    buf_puts(out, "    } else { /* send */\n");
    buf_puts(out, "        while (wch->count == wch->cap) pthread_cond_wait(&wch->not_full, &wch->lock);\n");
    buf_puts(out, "        wch->buf[wch->tail] = clauses[winner].val;\n");
    buf_puts(out, "        wch->tail = (wch->tail + 1) % wch->cap; wch->count++;\n");
    buf_puts(out, "        pthread_cond_signal(&wch->not_empty);\n");
    buf_puts(out, "        tur_waiter_signal_one(wch->recv_waiters);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    pthread_mutex_unlock(&wch->lock);\n");
    buf_puts(out, "    pthread_mutex_destroy(&wakeup_mutex);\n");
    buf_puts(out, "    pthread_cond_destroy(&wakeup_cond);\n");
    buf_puts(out, "    return winner;\n");
    buf_puts(out, "}\n\n");

    /* global_effect_handler_chain is declared earlier, before tur_fiber_block_new */
    buf_puts(out, "static int64_t tur_effect_perform(const char *name, int64_t *args, int n_args) {\n");
        /* T21-B: use fiber-local handler chain when inside a fiber */
    buf_puts(out, "    EffectHandlerFrame *frame =\n");
    buf_puts(out, "        (tur_current_fiber && tur_current_fiber->effect_handler_chain)\n");
    buf_puts(out, "        ? (EffectHandlerFrame *)tur_current_fiber->effect_handler_chain\n");
    buf_puts(out, "        : global_effect_handler_chain;\n");
    buf_puts(out, "    while (frame) {\n");
    buf_puts(out, "        for (int __i = 0; __i < frame->n_cases; __i++) {\n");
    buf_puts(out, "            if (strcmp(frame->cases[__i].effect_name, name) == 0) {\n");
    /* Phase 19D: intercept case - handler_fn == NULL means fiber-yield to parent dispatch loop */
    buf_puts(out, "                if (frame->cases[__i].handler_fn == NULL) {\n");
    buf_puts(out, "                    /* Phase 19D: intercept case - yield fiber to parent dispatch loop */\n");
    buf_puts(out, "                    FiberBlock *__cur = tur_current_fiber;\n");
    buf_puts(out, "                    if (!__cur || !__cur->eff_ctx) { fprintf(stderr, \"Unhandled effect: %s\\n\", name); abort(); }\n");
    buf_puts(out, "                    TurEffectCaptureCtx *__cap = (TurEffectCaptureCtx *)__cur->eff_ctx;\n");
    buf_puts(out, "                    __cap->eff_name = name;\n");
    buf_puts(out, "                    int __cn = n_args < 8 ? n_args : 8;\n");
    buf_puts(out, "                    for (int __ai = 0; __ai < __cn; __ai++) __cap->eff_args[__ai] = args[__ai];\n");
    buf_puts(out, "                    __cap->eff_n_args = n_args;\n");
    buf_puts(out, "                    __cap->has_pending_effect = true;\n");
    buf_puts(out, "                    tur_fiber_block_yield(0);\n");
    buf_puts(out, "                    __cap->has_pending_effect = false;\n");
    buf_puts(out, "                    return __cur->arg;\n");
    buf_puts(out, "                }\n");
    /* Phase P19-5: allocate a fresh TurContK on the stack per invocation */
    buf_puts(out, "                TurContK __fresh_k = {false, tur_current_fiber};\n");
    buf_puts(out, "                return frame->cases[__i].handler_fn(args, n_args, (int64_t)(intptr_t)&__fresh_k, frame->cases[__i].env);\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        frame = frame->parent;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    fprintf(stderr, \"Unhandled effect: %s\\n\", name);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n\n");

    /* FH3/FH5: generic dispatch loop for first-class handler values.
     * Identical in shape to the per-handle __dispatch_<id> emitted by
     * emit_effects_handle, but it scans a runtime tur_handler_table_t instead
     * of compile-time inline cases.  with-handler installs this as the
     * TurEffectCaptureCtx.dispatch fn and points ctx.table at the handler
     * value's table.  Multishot entries (cont_kind == CK_MULTISHOT) wrap the
     * fiber continuation in a cloneable cont, mirroring the inline path. */
    buf_puts(out, "struct __tur_msdyn_env { void *ctx; int64_t k_int; };\n");
    buf_puts(out, "static int64_t tur_handler_dispatch(void *__ctx_void, int64_t __k_int, int64_t __resume_val);\n");
    buf_puts(out, "static int64_t __tur_msdyn_cont(void *__env, int64_t __v) {\n");
    buf_puts(out, "    struct __tur_msdyn_env *__e = (struct __tur_msdyn_env *)__env;\n");
    buf_puts(out, "    return tur_handler_dispatch(__e->ctx, __e->k_int, __v);\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void *__tur_msdyn_clone(const void *__env) {\n");
    buf_puts(out, "    const struct __tur_msdyn_env *__o = (const struct __tur_msdyn_env *)__env;\n");
    buf_puts(out, "    struct __tur_msdyn_env *__c = (struct __tur_msdyn_env *)malloc(sizeof(struct __tur_msdyn_env));\n");
    buf_puts(out, "    if (__c) *__c = *__o;\n");
    buf_puts(out, "    return __c;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static int64_t tur_handler_dispatch(void *__ctx_void, int64_t __k_int, int64_t __resume_val) {\n");
    buf_puts(out, "    TurEffectCaptureCtx *__dcap = (TurEffectCaptureCtx *)__ctx_void;\n");
    buf_puts(out, "    FiberBlock *__fiber = (FiberBlock *)(intptr_t)__k_int;\n");
    buf_puts(out, "    int64_t __r = tur_fiber_block_resume(__fiber, __resume_val);\n");
    buf_puts(out, "    if (__fiber->done) { return __fiber->result; }\n");
    buf_puts(out, "    if (!__dcap->has_pending_effect) return __r;\n");
    buf_puts(out, "    tur_handler_table_t *__tbl = (tur_handler_table_t *)__dcap->table;\n");
    buf_puts(out, "    for (int __i = 0; __tbl && __i < __tbl->n_entries; __i++) {\n");
    buf_puts(out, "        if (strcmp(__dcap->eff_name, __tbl->entries[__i].eff_name) == 0) {\n");
    buf_puts(out, "            tur_handler_entry_t *__en = &__tbl->entries[__i];\n");
    /* The multishot branch references the cloneable-continuation runtime, which
     * is only emitted when the program uses multishot/cloneable continuations.
     * Gate it on the same predicate; otherwise multishot dispatch is unreachable
     * (a multishot handler literal makes the predicate true via
     * expr_has_multishot_handler). */
    if (cps_expr_contains_cloneable_shift(program) || expr_has_multishot_handler(program)) {
    buf_printf(out, "            if (__en->cont_kind == %d) { /* CK_MULTISHOT */\n", (int)CK_MULTISHOT);
    buf_puts(out, "                struct __tur_msdyn_env *__ms = (struct __tur_msdyn_env *)malloc(sizeof(struct __tur_msdyn_env));\n");
    buf_puts(out, "                if (!__ms) abort();\n");
    buf_puts(out, "                __ms->ctx = __ctx_void; __ms->k_int = __k_int;\n");
    buf_puts(out, "                int64_t __k_ms = (int64_t)(intptr_t)tur_cloneable_cont_alloc(__tur_msdyn_cont, __ms, __tur_msdyn_clone, free);\n");
    buf_puts(out, "                return __en->fn(__dcap->eff_args, __dcap->eff_n_args, __k_ms, __en->env);\n");
    buf_puts(out, "            }\n");
    }
    buf_puts(out, "            return __en->fn(__dcap->eff_args, __dcap->eff_n_args, __k_int, __en->env);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    /* Bubble unhandled effect to the outer fiber, mirroring the per-handle path. */
    buf_puts(out, "    FiberBlock *__outer_f = tur_current_fiber;\n");
    buf_puts(out, "    if (__outer_f && __outer_f->eff_ctx) {\n");
    buf_puts(out, "        TurEffectCaptureCtx *__oc = (TurEffectCaptureCtx *)__outer_f->eff_ctx;\n");
    buf_puts(out, "        __oc->eff_name = __dcap->eff_name;\n");
    buf_puts(out, "        int __bn = __dcap->eff_n_args < 8 ? __dcap->eff_n_args : 8;\n");
    buf_puts(out, "        for (int __bi = 0; __bi < __bn; __bi++) __oc->eff_args[__bi] = __dcap->eff_args[__bi];\n");
    buf_puts(out, "        __oc->eff_n_args = __dcap->eff_n_args;\n");
    buf_puts(out, "        __oc->has_pending_effect = true;\n");
    buf_puts(out, "        tur_fiber_block_yield(0);\n");
    buf_puts(out, "        __oc->has_pending_effect = false;\n");
    buf_puts(out, "        return tur_handler_dispatch(__ctx_void, __k_int, __outer_f->arg);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    fprintf(stderr, \"dispatch: unhandled effect: %s\\n\", __dcap->eff_name);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n\n");

    /* Phase 9: Emit rc.h inline for rc<T> + weak<T> support */
    /* Phase 10: GC color enum and runtime (needed by rc_cb_alloc) */
    buf_puts(out, "/* rc<T> + weak<T> reference counting - Phase 9 */\n");
    buf_puts(out, "/* Phase 10: GC color enum for Bacon-Rajan */\n");
    buf_puts(out, "typedef enum { GC_WHITE, GC_GREY, GC_BLACK, GC_PURPLE } GcColor;\n\n");
    buf_puts(out, "typedef void (*RcDropFn)(void *value);\n\n");
    buf_puts(out, "typedef struct RcControlBlock RcControlBlock;\n\n");
    /* DS3: walker callbacks for the cycle collector (RCK_STRUCT). */
    buf_puts(out, "typedef void (*RcWalkChildFn)(RcControlBlock *child, void *ctx);\n");
    buf_puts(out, "typedef void (*RcWalkFn)(void *value, RcWalkChildFn cb, void *ctx);\n\n");
    buf_puts(out, "struct RcControlBlock {\n");
    buf_puts(out, "    uint64_t strong_count;\n");
    buf_puts(out, "    uint64_t weak_count;\n");
    buf_puts(out, "    void *value;\n");
    buf_puts(out, "    RcDropFn drop_fn;\n");
    /* DS3: walker function for RCK_STRUCT blocks (NULL otherwise). */
    buf_puts(out, "    RcWalkFn walk_fn;\n");
    buf_puts(out, "    uint8_t value_type_kind;\n");
    /* Phase 10: Bacon-Rajan GC fields */
    buf_puts(out, "    uint8_t color;           /* GC color */\n");
    buf_puts(out, "    bool may_contain_cycles;  /* Hint for GC */\n");
    buf_puts(out, "    uint8_t reserved[6];\n");
    buf_puts(out, "};\n\n");
    /* Phase 9: Deferred free queue to avoid deep recursion in rc_strong_decrement */
    buf_puts(out, "#define RC_FREE_QUEUE_CAPACITY 65536\n");
    buf_puts(out, "static RcControlBlock *rc_free_queue[RC_FREE_QUEUE_CAPACITY];\n");
    buf_puts(out, "static uint32_t rc_free_queue_count = 0;\n");
    buf_puts(out, "static uint32_t rc_free_queue_drain(void);  /* Forward decl */\n");
    buf_puts(out, "static void rc_free_queue_push(RcControlBlock *cb);  /* Forward decl */\n");
    buf_puts(out, "bool rc_strong_decrement(RcControlBlock *cb);  /* Forward decl */\n");
    buf_puts(out, "bool rc_weak_decrement(RcControlBlock *cb);    /* Forward decl */\n");
    buf_puts(out, "static void rc_free_queue_push(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    if (rc_free_queue_count >= RC_FREE_QUEUE_CAPACITY) {\n");
    buf_puts(out, "        fprintf(stderr, \"rc_free_queue: full, aborting\\n\");\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    rc_free_queue[rc_free_queue_count++] = cb;\n");
    buf_puts(out, "}\n\n");
    /* Phase 10: GC globals and helper functions (needed before rc_cb_alloc) */
    buf_puts(out, "#define GC_GLOBAL_REGISTRY_CAPACITY 4096\n");
    buf_puts(out, "static RcControlBlock *gc_all_blocks[GC_GLOBAL_REGISTRY_CAPACITY];\n");
    buf_puts(out, "static uint32_t gc_all_blocks_count = 0;\n\n");
    /* GC state must be declared early because gc_on_strong_decrement (called from
     * rc_strong_decrement) needs it.  The collector functions themselves are
     * defined later, after all RC helpers. */
    buf_puts(out, "#define GC_SUSPECT_THRESHOLD 128\n");
    buf_puts(out, "#define GC_MAX_SUSPECTS 4096\n\n");
    buf_puts(out, "typedef enum { GC_DISABLED, GC_MANUAL, GC_THRESHOLD } GcMode;\n\n");
    buf_puts(out, "static RcControlBlock *gc_suspect_roots[GC_MAX_SUSPECTS];\n");
    buf_puts(out, "static uint32_t gc_suspect_count = 0;\n");
    buf_puts(out, "static RcControlBlock *gc_grey_queue[GC_MAX_SUSPECTS];\n");
    buf_puts(out, "static uint32_t gc_grey_count = 0;\n");
    buf_puts(out, "static GcMode gc_mode = GC_DISABLED;\n");
    buf_puts(out, "static bool gc_enabled = false;\n\n");
    buf_puts(out, "static void gc_collect(void);  /* Forward decl */\n\n");
    buf_puts(out, "static void gc_set_color(RcControlBlock *cb, GcColor color) {\n");
    buf_puts(out, "    if (cb) cb->color = color;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static GcColor gc_get_color(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (cb) return cb->color;\n");
    buf_puts(out, "    return GC_WHITE;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_register_block(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb || gc_all_blocks_count >= GC_GLOBAL_REGISTRY_CAPACITY) return;\n");
    buf_puts(out, "    gc_all_blocks[gc_all_blocks_count++] = cb;\n");
    buf_puts(out, "    cb->color = GC_WHITE;\n");
    buf_puts(out, "    cb->may_contain_cycles = true;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_unregister_block(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {\n");
    buf_puts(out, "        if (gc_all_blocks[i] == cb) {\n");
    buf_puts(out, "            gc_all_blocks[i] = gc_all_blocks[gc_all_blocks_count - 1];\n");
    buf_puts(out, "            gc_all_blocks_count--;\n");
    buf_puts(out, "            break;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    /* Phase 10: Suspect buffer management (before rc_strong_decrement) */
    buf_puts(out, "static void gc_add_suspect(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb || !gc_enabled || gc_mode == GC_DISABLED) return;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_suspect_count; i++) {\n");
    buf_puts(out, "        if (gc_suspect_roots[i] == cb) return;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (gc_suspect_count >= GC_MAX_SUSPECTS) return;\n");
    buf_puts(out, "    gc_suspect_roots[gc_suspect_count++] = cb;\n");
    buf_puts(out, "    cb->color = GC_PURPLE;\n");
    buf_puts(out, "    /* Threshold mode: auto-collect when buffer is full */\n");
    buf_puts(out, "    if (gc_mode == GC_THRESHOLD && gc_suspect_count >= GC_SUSPECT_THRESHOLD) {\n");
    buf_puts(out, "        gc_collect();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_remove_suspect(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_suspect_count; i++) {\n");
    buf_puts(out, "        if (gc_suspect_roots[i] == cb) {\n");
    buf_puts(out, "            gc_suspect_roots[i] = gc_suspect_roots[gc_suspect_count - 1];\n");
    buf_puts(out, "            gc_suspect_count--;\n");
    buf_puts(out, "            return;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_on_strong_decrement(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    /* Zombie: strong reached 0 but weak refs still exist */\n");
    buf_puts(out, "    if (cb->weak_count > 0) {\n");
    buf_puts(out, "        gc_add_suspect(cb);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "\n/* Phase 9: Deferred free queue drain */\n");
    buf_puts(out, "static uint32_t rc_free_queue_drain(void) {\n");
    buf_puts(out, "    if (rc_free_queue_count == 0) return 0;\n");
    buf_puts(out, "    uint32_t freed = 0;\n");
    buf_puts(out, "    while (rc_free_queue_count > 0) {\n");
    buf_puts(out, "        RcControlBlock *cb = rc_free_queue[0];\n");
    buf_puts(out, "        memmove(rc_free_queue, rc_free_queue + 1,\n");
    buf_puts(out, "                (rc_free_queue_count - 1) * sizeof(RcControlBlock *));\n");
    buf_puts(out, "        rc_free_queue_count--;\n");
    buf_puts(out, "        gc_unregister_block(cb);\n");
    /* F1-2-4: smart drop dispatch for RCK_EXISTENTIAL blocks whose
     * payload is itself an rc reference (RCEXP_RC).  See rc_cb_free in
     * src/runtime/rc.c for the matching runtime-library code. */
    buf_puts(out, "        if (cb->value && cb->reserved[0] == 1 /* RCK_EXISTENTIAL */ && cb->reserved[1] == 1 /* RCEXP_RC */) {\n");
    buf_puts(out, "            int64_t __raw = *(const int64_t *)cb->value;\n");
    buf_puts(out, "            RcControlBlock *__inner = (RcControlBlock *)(intptr_t)__raw;\n");
    buf_puts(out, "            if (__inner) rc_strong_decrement(__inner);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        if (cb->value) cb->drop_fn(cb->value);\n");
    buf_puts(out, "        free(cb);\n");
    buf_puts(out, "        freed++;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return freed;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void default_rc_drop_fn(void *value) {\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void drop_ref_payload(void *value) {\n");
    buf_puts(out, "    if (!value) return;\n");
    buf_puts(out, "    void *inner = *((void **)value);\n");
    buf_puts(out, "    if (inner) free(inner);\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void drop_rc_payload(void *value) {\n");
    buf_puts(out, "    if (!value) return;\n");
    buf_puts(out, "    RcControlBlock *inner = *((RcControlBlock **)value);\n");
    buf_puts(out, "    if (inner) { rc_strong_decrement(inner); rc_free_queue_drain(); }\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void drop_weak_payload(void *value) {\n");
    buf_puts(out, "    if (!value) return;\n");
    buf_puts(out, "    RcControlBlock *inner = *((RcControlBlock **)value);\n");
    buf_puts(out, "    if (inner) rc_weak_decrement(inner);\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static RcDropFn default_drop_fn_for_type(int value_type_kind) {\n");
    buf_puts(out, "    switch (value_type_kind) {\n");
    buf_puts(out, "        case 8: return drop_ref_payload;   /* TY_REF */\n");
    buf_puts(out, "        case 9: return drop_rc_payload;    /* TY_RC */\n");
    buf_puts(out, "        case 10: return drop_weak_payload; /* TY_WEAK */\n");
    buf_puts(out, "        default: return default_rc_drop_fn;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    /* EXG5: layout-tag constants kept in sync with runtime/rc.h. */
    buf_puts(out, "#define RCK_OPAQUE       0\n");
    buf_puts(out, "#define RCK_EXISTENTIAL  1\n");
    buf_puts(out, "#define RCK_STRUCT       2\n");
    buf_puts(out, "#define RCEXP_OPAQUE     0\n");
    buf_puts(out, "#define RCEXP_RC         1\n");
    buf_puts(out, "RcControlBlock *rc_cb_alloc_kinded(size_t value_size, int value_type_kind, RcDropFn drop_fn, uint8_t kind, uint8_t payload_kind) {\n");
    buf_puts(out, "    size_t total_size = sizeof(RcControlBlock) + value_size;\n");
    buf_puts(out, "    RcControlBlock *cb = (RcControlBlock *)malloc(total_size);\n");
    buf_puts(out, "    if (!cb) { fprintf(stderr, \"rc: out of memory\\n\"); abort(); }\n");
    buf_puts(out, "    cb->strong_count = 1;\n");
    buf_puts(out, "    cb->weak_count = 0;\n");
    buf_puts(out, "    cb->value = (void *)(cb + 1);\n");
    buf_puts(out, "    cb->drop_fn = drop_fn ? drop_fn : default_drop_fn_for_type(value_type_kind);\n");
    buf_puts(out, "    cb->walk_fn = NULL;\n");
    buf_puts(out, "    cb->value_type_kind = value_type_kind;\n");
    buf_puts(out, "    memset(cb->reserved, 0, sizeof(cb->reserved));\n");
    buf_puts(out, "    cb->reserved[0] = kind;\n");
    buf_puts(out, "    cb->reserved[1] = payload_kind;\n");
    buf_puts(out, "    /* Register with GC; primitives (type_kind<=7) cannot form cycles */\n");
    buf_puts(out, "    gc_register_block(cb);\n");
    buf_puts(out, "    if (value_type_kind <= 7) cb->may_contain_cycles = false;\n");
    buf_puts(out, "    return cb;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "RcControlBlock *rc_cb_alloc(size_t value_size, int value_type_kind, RcDropFn drop_fn) {\n");
    buf_puts(out, "    return rc_cb_alloc_kinded(value_size, value_type_kind, drop_fn, RCK_OPAQUE, RCEXP_OPAQUE);\n");
    buf_puts(out, "}\n\n");
    /* DS3: RCK_STRUCT variant -- attaches a walk_fn for the cycle collector. */
    buf_puts(out, "RcControlBlock *rc_cb_alloc_struct(size_t value_size, int value_type_kind, RcDropFn drop_fn, RcWalkFn walk_fn) {\n");
    buf_puts(out, "    RcControlBlock *cb = rc_cb_alloc_kinded(value_size, value_type_kind, drop_fn, RCK_STRUCT, 0);\n");
    buf_puts(out, "    cb->walk_fn = walk_fn;\n");
    buf_puts(out, "    return cb;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_strong_increment(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return ++cb->strong_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "bool rc_strong_decrement(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    cb->strong_count--;\n");
    buf_puts(out, "    if (cb->strong_count == 0) {\n");
    buf_puts(out, "        if (cb->weak_count > 0) {\n");
    buf_puts(out, "            gc_on_strong_decrement(cb);\n");
    buf_puts(out, "            return false;\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            rc_free_queue_push(cb);\n");
    buf_puts(out, "            return true;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return false;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_weak_increment(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return ++cb->weak_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "bool rc_weak_decrement(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    cb->weak_count--;\n");
    buf_puts(out, "    if (cb->weak_count == 0 && cb->strong_count == 0) {\n");
    buf_puts(out, "        gc_unregister_block(cb);\n");
    /* F1-2-4: same smart-drop dispatch as rc_free_queue_drain.  The
     * weak path runs when the last weak ref is released after the
     * value already reached zombie state; if the zombie's payload is
     * an RCEXP_RC pointer, we still need to decrement the inner ref
     * before freeing the outer block. */
    buf_puts(out, "        if (cb->value && cb->reserved[0] == 1 /* RCK_EXISTENTIAL */ && cb->reserved[1] == 1 /* RCEXP_RC */) {\n");
    buf_puts(out, "            int64_t __raw = *(const int64_t *)cb->value;\n");
    buf_puts(out, "            RcControlBlock *__inner = (RcControlBlock *)(intptr_t)__raw;\n");
    buf_puts(out, "            if (__inner) rc_strong_decrement(__inner);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        /* Free zombie value if GC did not already collect it */\n");
    buf_puts(out, "        if (cb->value && cb->drop_fn) {\n");
    buf_puts(out, "            cb->drop_fn(cb->value);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        free(cb);\n");
    buf_puts(out, "        return true;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return false;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_strong_count(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return cb->strong_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_weak_count(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return cb->weak_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "bool rc_is_alive(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    return cb->strong_count > 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "RcControlBlock *rc_upgrade(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return NULL;\n");
    buf_puts(out, "    if (cb->strong_count > 0) {\n");
    buf_puts(out, "        rc_strong_increment(cb);\n");
    buf_puts(out, "        return cb;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return NULL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "void *rc_get_value(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return NULL;\n");
    buf_puts(out, "    return cb->value;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "RcControlBlock *tur_rc_from_ref(void *ref_value, int value_type_kind) {\n");
    buf_puts(out, "    if (!ref_value) return NULL;\n");
    buf_puts(out, "    RcControlBlock *cb = (RcControlBlock *)malloc(sizeof(RcControlBlock));\n");
    buf_puts(out, "    if (!cb) { fprintf(stderr, \"rc/from-ref: out of memory\\n\"); abort(); }\n");
    buf_puts(out, "    cb->strong_count = 1;\n");
    buf_puts(out, "    cb->weak_count = 0;\n");
    buf_puts(out, "    cb->value = ref_value;\n");
    buf_puts(out, "    cb->drop_fn = default_drop_fn_for_type(value_type_kind);\n");
    buf_puts(out, "    cb->value_type_kind = (uint8_t)value_type_kind;\n");
    buf_puts(out, "    cb->color = GC_WHITE;\n");
    buf_puts(out, "    cb->may_contain_cycles = true;\n");
    buf_puts(out, "    memset(cb->reserved, 0, sizeof(cb->reserved));\n");
    buf_puts(out, "    gc_register_block(cb);\n");
    buf_puts(out, "    return cb;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "void *tur_ref_from_rc(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return NULL;\n");
    buf_puts(out, "    if (cb->strong_count != 1 || cb->weak_count != 0) {\n");
    buf_puts(out, "        fprintf(stderr, \"ref/from-rc requires unique rc (strong_count==1 and weak_count==0), got strong=%llu weak=%llu\\n\",\n");
    buf_puts(out, "                (unsigned long long)cb->strong_count, (unsigned long long)cb->weak_count);\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    void *value = cb->value;\n");
    buf_puts(out, "    cb->value = NULL;\n");
    buf_puts(out, "    gc_unregister_block(cb);\n");
    buf_puts(out, "    free(cb);\n");
    buf_puts(out, "    return value;\n");
    buf_puts(out, "}\n\n");
    
    /* Phase 10: Emit remaining GC runtime — mark + trial deletion phases */
    buf_puts(out, "/* gc (Bacon-Rajan cycle collector - trial deletion) - Phase 10 */\n");
    /* DS3: child-mark callback used by RCK_STRUCT walk_fns inside gc_mark_phase. */
    buf_puts(out, "static void __gc_mark_struct_child(RcControlBlock *child, void *ctx) {\n");
    buf_puts(out, "    (void)ctx;\n");
    buf_puts(out, "    if (child && child->color != GC_BLACK) {\n");
    buf_puts(out, "        child->color = GC_BLACK;\n");
    buf_puts(out, "        if (gc_grey_count < GC_MAX_SUSPECTS) {\n");
    buf_puts(out, "            gc_grey_queue[gc_grey_count++] = child;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_mark_phase(void) {\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {\n");
    buf_puts(out, "        gc_all_blocks[i]->color = GC_WHITE;\n");
    buf_puts(out, "    }\n");
    /* Initial sweep: every strong-rooted block is BLACK and enters the
     * grey queue so EXG5 propagation below can chase its outgoing edges. */
    buf_puts(out, "    gc_grey_count = 0;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {\n");
    buf_puts(out, "        RcControlBlock *cb = gc_all_blocks[i];\n");
    buf_puts(out, "        if (cb->strong_count > 0) {\n");
    buf_puts(out, "            cb->color = GC_BLACK;\n");
    buf_puts(out, "            if (gc_grey_count < GC_MAX_SUSPECTS) {\n");
    buf_puts(out, "                gc_grey_queue[gc_grey_count++] = cb;\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    /* EXG5: propagate reachability through known layouts.  Today only
     * RCK_EXISTENTIAL blocks expose a follow-able payload (the inner
     * RcControlBlock pointer when payload_kind == RCEXP_RC).  Without
     * this step the walker would treat the inner rc as garbage even
     * while the outer existential is strongly held. */
    buf_puts(out, "    while (gc_grey_count > 0) {\n");
    buf_puts(out, "        RcControlBlock *cb = gc_grey_queue[--gc_grey_count];\n");
    buf_puts(out, "        if (cb && cb->value && cb->reserved[0] == RCK_EXISTENTIAL && cb->reserved[1] == RCEXP_RC) {\n");
    buf_puts(out, "            int64_t raw = *(const int64_t *)cb->value;\n");
    buf_puts(out, "            RcControlBlock *inner = (RcControlBlock *)(intptr_t)raw;\n");
    buf_puts(out, "            if (inner && inner->color != GC_BLACK) {\n");
    buf_puts(out, "                inner->color = GC_BLACK;\n");
    buf_puts(out, "                if (gc_grey_count < GC_MAX_SUSPECTS) {\n");
    buf_puts(out, "                    gc_grey_queue[gc_grey_count++] = inner;\n");
    buf_puts(out, "                }\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "        } else if (cb && cb->value && cb->reserved[0] == RCK_STRUCT && cb->walk_fn) {\n");
    buf_puts(out, "            cb->walk_fn(cb->value, __gc_mark_struct_child, NULL);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    /* Trial deletion: free zombie suspects not reachable from strong roots */
    buf_puts(out, "static void gc_trial_deletion_phase(void) {\n");
    buf_puts(out, "    uint32_t i = 0;\n");
    buf_puts(out, "    while (i < gc_suspect_count) {\n");
    buf_puts(out, "        RcControlBlock *cb = gc_suspect_roots[i];\n");
    buf_puts(out, "        /* If revived or reachable from strong roots, keep */\n");
    buf_puts(out, "        if (cb->strong_count > 0 || cb->color == GC_BLACK) {\n");
    buf_puts(out, "            cb->color = GC_WHITE;\n");
    buf_puts(out, "            i++;\n");
    buf_puts(out, "            continue;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        /* WHITE and strong_count == 0: zombie — free value, keep cb for weak refs */\n");
    buf_puts(out, "        if (cb->value && cb->drop_fn) {\n");
    buf_puts(out, "            cb->drop_fn(cb->value);\n");
    buf_puts(out, "            cb->value = NULL;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        /* Unregister from global registry (cb stays alive for weak refs) */\n");
    buf_puts(out, "        gc_unregister_block(cb);\n");
    buf_puts(out, "        /* Remove from suspect buffer without advancing i */\n");
    buf_puts(out, "        gc_suspect_roots[i] = gc_suspect_roots[gc_suspect_count - 1];\n");
    buf_puts(out, "        gc_suspect_count--;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_collect(void) {\n");
    buf_puts(out, "    if (!gc_enabled || gc_mode == GC_DISABLED) return;\n");
    buf_puts(out, "    gc_grey_count = 0;\n");
    buf_puts(out, "    gc_mark_phase();\n");
    buf_puts(out, "    gc_trial_deletion_phase();\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_force(void) {\n");
    buf_puts(out, "    gc_collect();\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_enable(void) {\n");
    buf_puts(out, "    gc_enabled = true;\n");
    buf_puts(out, "    /* Default to manual mode when enabled */\n");
    buf_puts(out, "    if (gc_mode == GC_DISABLED) gc_mode = GC_MANUAL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_disable(void) {\n");
    buf_puts(out, "    gc_enabled = false;\n");
    buf_puts(out, "    gc_mode = GC_DISABLED;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_set_mode(GcMode mode) {\n");
    buf_puts(out, "    gc_mode = mode;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static bool gc_is_alive(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    if (cb->strong_count > 0) return true;\n");
    buf_puts(out, "    return (cb->color == GC_BLACK || cb->color == GC_GREY);\n");
    buf_puts(out, "}\n\n");

    /* SS2: TurChannel -- synchronous rendezvous channel for session types.
     * Only emitted when -Xsessions is active. */
    if (g_sessions_enabled) {
        buf_puts(out, "/* SS2: TurChannel -- synchronous rendezvous channel for session types */\n");
        buf_puts(out, "#ifndef NDEBUG\n");
        buf_puts(out, "#  define TUR_DBGPROTO(s) (s)\n");
        buf_puts(out, "#else\n");
        buf_puts(out, "#  define TUR_DBGPROTO(s) ((const char*)0)\n");
        buf_puts(out, "#endif\n");
        /* TurSyncCh: synchronous rendezvous slot with explicit send/recv handshake.
         * state: 0=idle, 1=sender deposited (waiting for recv), 2=receiver acked (waiting for send to return)
         * cv: broadcast on every state change; both sender and receiver use it.
         * The three-state design ensures no ABA races when the same thread
         * receives a value and immediately sends a new one on the same channel. */
        buf_puts(out, "typedef struct {\n");
        buf_puts(out, "    pthread_mutex_t mu;\n");
        buf_puts(out, "    pthread_cond_t  cv;\n");
        buf_puts(out, "    int64_t val;\n");
        buf_puts(out, "    int state; /* 0=idle 1=data-ready 2=data-acked */\n");
        buf_puts(out, "} TurSyncCh;\n");
        buf_puts(out, "typedef struct {\n");
        buf_puts(out, "    TurSyncCh data;\n");
        buf_puts(out, "    TurSyncCh branch;\n");
        buf_puts(out, "    int refcount;\n");
        buf_puts(out, "    int abandoned; /* set when timeout fires; unblocks sender */\n");
        buf_puts(out, "    pthread_mutex_t rc_mu;\n");
        buf_puts(out, "#ifndef NDEBUG\n");
        buf_puts(out, "    const char *dbg_proto;\n");
        buf_puts(out, "#endif\n");
        buf_puts(out, "} TurChannel;\n");
        buf_puts(out, "static TurChannel *tur_session_new(const char *proto) {\n");
        buf_puts(out, "    TurChannel *ch = (TurChannel *)calloc(1, sizeof(TurChannel));\n");
        buf_puts(out, "    pthread_mutex_init(&ch->data.mu, NULL);\n");
        buf_puts(out, "    pthread_cond_init(&ch->data.cv, NULL);\n");
        buf_puts(out, "    pthread_mutex_init(&ch->branch.mu, NULL);\n");
        buf_puts(out, "    pthread_cond_init(&ch->branch.cv, NULL);\n");
        buf_puts(out, "    pthread_mutex_init(&ch->rc_mu, NULL);\n");
        buf_puts(out, "    ch->refcount = 2;\n");
        buf_puts(out, "#ifndef NDEBUG\n");
        buf_puts(out, "    ch->dbg_proto = proto;\n");
        buf_puts(out, "#else\n");
        buf_puts(out, "    (void)proto;\n");
        buf_puts(out, "#endif\n");
        buf_puts(out, "    return ch;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void tur_session_send(TurChannel *ch, int64_t val) {\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->data.mu);\n");
        buf_puts(out, "    /* phase 1: wait for idle slot, deposit value */\n");
        buf_puts(out, "    while (ch->data.state != 0 && !ch->abandoned)\n");
        buf_puts(out, "        pthread_cond_wait(&ch->data.cv, &ch->data.mu);\n");
        buf_puts(out, "    if (ch->abandoned) { pthread_mutex_unlock(&ch->data.mu); return; }\n");
        buf_puts(out, "    ch->data.val = val; ch->data.state = 1;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->data.cv);\n");
        buf_puts(out, "    /* phase 2: wait for receiver ack (state==2), then clear to idle */\n");
        buf_puts(out, "    while (ch->data.state != 2 && !ch->abandoned)\n");
        buf_puts(out, "        pthread_cond_wait(&ch->data.cv, &ch->data.mu);\n");
        buf_puts(out, "    if (!ch->abandoned) {\n");
        buf_puts(out, "        ch->data.state = 0;\n");
        buf_puts(out, "        pthread_cond_broadcast(&ch->data.cv);\n");
        buf_puts(out, "    }\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->data.mu);\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static int64_t tur_session_recv(TurChannel *ch) {\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->data.mu);\n");
        buf_puts(out, "    /* wait for data-ready (state==1), read, ack */\n");
        buf_puts(out, "    while (ch->data.state != 1) pthread_cond_wait(&ch->data.cv, &ch->data.mu);\n");
        buf_puts(out, "    int64_t v = ch->data.val; ch->data.state = 2;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->data.cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->data.mu);\n");
        buf_puts(out, "    return v;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void tur_session_send_tag(TurChannel *ch, int64_t tag) {\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->branch.mu);\n");
        buf_puts(out, "    while (ch->branch.state != 0) pthread_cond_wait(&ch->branch.cv, &ch->branch.mu);\n");
        buf_puts(out, "    ch->branch.val = tag; ch->branch.state = 1;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->branch.cv);\n");
        buf_puts(out, "    while (ch->branch.state != 2) pthread_cond_wait(&ch->branch.cv, &ch->branch.mu);\n");
        buf_puts(out, "    ch->branch.state = 0;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->branch.cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->branch.mu);\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static int64_t tur_session_recv_tag(TurChannel *ch) {\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->branch.mu);\n");
        buf_puts(out, "    while (ch->branch.state != 1) pthread_cond_wait(&ch->branch.cv, &ch->branch.mu);\n");
        buf_puts(out, "    int64_t tag = ch->branch.val; ch->branch.state = 2;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->branch.cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->branch.mu);\n");
        buf_puts(out, "    return tag;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void tur_session_close(TurChannel *ch) {\n");
        buf_puts(out, "    /* signal abandoned so any blocked sender/sender_tag wakes up */\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->data.mu);\n");
        buf_puts(out, "    ch->abandoned = 1;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->data.cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->data.mu);\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->rc_mu);\n");
        buf_puts(out, "    int rc = --ch->refcount;\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->rc_mu);\n");
        buf_puts(out, "    if (rc == 0) {\n");
        buf_puts(out, "        pthread_mutex_destroy(&ch->data.mu); pthread_cond_destroy(&ch->data.cv);\n");
        buf_puts(out, "        pthread_mutex_destroy(&ch->branch.mu); pthread_cond_destroy(&ch->branch.cv);\n");
        buf_puts(out, "        pthread_mutex_destroy(&ch->rc_mu); free(ch);\n");
        buf_puts(out, "    }\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void *tur_session_thread_wrapper(void *arg) {\n");
        buf_puts(out, "    int64_t *fat = (int64_t *)arg;\n");
        buf_puts(out, "    int64_t (*thunk)(void *) = (int64_t (*)(void *))(intptr_t)fat[0];\n");
        buf_puts(out, "    thunk(arg);\n");
        buf_puts(out, "    return NULL;\n");
        buf_puts(out, "}\n");
        /* SS3c: thread-local storage for recv-timeout value. */
        buf_puts(out, "static _Thread_local int64_t tur__rtv_ = 0;\n");
        buf_puts(out, "static int64_t tur_session_recv_timeout(TurChannel *ch, int64_t ms) {\n");
        buf_puts(out, "    struct timespec ts;\n");
        buf_puts(out, "    clock_gettime(CLOCK_REALTIME, &ts);\n");
        buf_puts(out, "    ts.tv_sec  += (time_t)(ms / 1000);\n");
        buf_puts(out, "    ts.tv_nsec += (long)((ms % 1000) * 1000000L);\n");
        buf_puts(out, "    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->data.mu);\n");
        buf_puts(out, "    int rc = 0;\n");
        buf_puts(out, "    while (ch->data.state != 1 && rc == 0)\n");
        buf_puts(out, "        rc = pthread_cond_timedwait(&ch->data.cv, &ch->data.mu, &ts);\n");
        buf_puts(out, "    int64_t tag;\n");
        buf_puts(out, "    if (ch->data.state == 1) {\n");
        buf_puts(out, "        tur__rtv_ = ch->data.val; ch->data.state = 2;\n");
        buf_puts(out, "        pthread_cond_broadcast(&ch->data.cv);\n");
        buf_puts(out, "        tag = 0;\n");
        buf_puts(out, "    } else {\n");
        buf_puts(out, "        tag = 1;\n");
        buf_puts(out, "    }\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->data.mu);\n");
        buf_puts(out, "    return tag;\n");
        buf_puts(out, "}\n\n");
        /* SS7: Multi-party session router (TurRouter / TurRole). */
        buf_puts(out, "/* SS7: Multi-party session router.\n");
        buf_puts(out, " * TurRouter: holds N*N TurSyncCh slots (i,j) = slot[i*n_roles+j], i!=j used.\n");
        buf_puts(out, " * refcount = number of live TurRole endpoints; freed when it hits 0. */\n");
        buf_puts(out, "typedef struct {\n");
        buf_puts(out, "    int n_roles;\n");
        buf_puts(out, "    int refcount;\n");
        buf_puts(out, "    pthread_mutex_t rc_mu;\n");
        buf_puts(out, "    TurSyncCh slots[1]; /* flexible: actually n_roles*n_roles entries */\n");
        buf_puts(out, "} TurRouter;\n");
        buf_puts(out, "typedef struct {\n");
        buf_puts(out, "    TurRouter *router;\n");
        buf_puts(out, "    int role_idx;\n");
        buf_puts(out, "} TurRole;\n");
        buf_puts(out, "static TurRouter *tur_router_new(int n) {\n");
        buf_puts(out, "    size_t sz = sizeof(TurRouter) + (size_t)((n * n) - 1) * sizeof(TurSyncCh);\n");
        buf_puts(out, "    TurRouter *r = (TurRouter *)calloc(1, sz);\n");
        buf_puts(out, "    r->n_roles  = n;\n");
        buf_puts(out, "    r->refcount = n;\n");
        buf_puts(out, "    pthread_mutex_init(&r->rc_mu, NULL);\n");
        buf_puts(out, "    for (int i = 0; i < n * n; i++) {\n");
        buf_puts(out, "        pthread_mutex_init(&r->slots[i].mu, NULL);\n");
        buf_puts(out, "        pthread_cond_init(&r->slots[i].cv, NULL);\n");
        buf_puts(out, "    }\n");
        buf_puts(out, "    return r;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void *tur_make_roles(int n, int idx) {\n");
        buf_puts(out, "    TurRouter *r = tur_router_new(n);\n");
        buf_puts(out, "    TurRole *role = (TurRole *)malloc(sizeof(TurRole));\n");
        buf_puts(out, "    role->router   = r;\n");
        buf_puts(out, "    role->role_idx = idx;\n");
        buf_puts(out, "    return (void *)role;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void *tur_get_role(void *base, int peer_idx) {\n");
        buf_puts(out, "    TurRole *b = (TurRole *)base;\n");
        buf_puts(out, "    TurRole *role = (TurRole *)malloc(sizeof(TurRole));\n");
        buf_puts(out, "    role->router   = b->router;\n");
        buf_puts(out, "    role->role_idx = peer_idx;\n");
        buf_puts(out, "    return (void *)role;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void tur_router_send(void *role_ptr, int to_idx, int64_t val) {\n");
        buf_puts(out, "    TurRole *role = (TurRole *)role_ptr;\n");
        buf_puts(out, "    TurSyncCh *ch = &role->router->slots[role->role_idx * role->router->n_roles + to_idx];\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->mu);\n");
        buf_puts(out, "    while (ch->state != 0) pthread_cond_wait(&ch->cv, &ch->mu);\n");
        buf_puts(out, "    ch->val = val; ch->state = 1;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->cv);\n");
        buf_puts(out, "    while (ch->state != 2) pthread_cond_wait(&ch->cv, &ch->mu);\n");
        buf_puts(out, "    ch->state = 0;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->mu);\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static int64_t tur_router_recv(void *role_ptr, int from_idx) {\n");
        buf_puts(out, "    TurRole *role = (TurRole *)role_ptr;\n");
        buf_puts(out, "    TurSyncCh *ch = &role->router->slots[from_idx * role->router->n_roles + role->role_idx];\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->mu);\n");
        buf_puts(out, "    while (ch->state != 1) pthread_cond_wait(&ch->cv, &ch->mu);\n");
        buf_puts(out, "    int64_t v = ch->val; ch->state = 2;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->mu);\n");
        buf_puts(out, "    return v;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void tur_role_close(void *role_ptr) {\n");
        buf_puts(out, "    TurRole *role = (TurRole *)role_ptr;\n");
        buf_puts(out, "    pthread_mutex_lock(&role->router->rc_mu);\n");
        buf_puts(out, "    int rc = --role->router->refcount;\n");
        buf_puts(out, "    pthread_mutex_unlock(&role->router->rc_mu);\n");
        buf_puts(out, "    if (rc == 0) {\n");
        buf_puts(out, "        int n = role->router->n_roles;\n");
        buf_puts(out, "        for (int i = 0; i < n * n; i++) {\n");
        buf_puts(out, "            pthread_mutex_destroy(&role->router->slots[i].mu);\n");
        buf_puts(out, "            pthread_cond_destroy(&role->router->slots[i].cv);\n");
        buf_puts(out, "        }\n");
        buf_puts(out, "        pthread_mutex_destroy(&role->router->rc_mu);\n");
        buf_puts(out, "        free(role->router);\n");
        buf_puts(out, "    }\n");
        buf_puts(out, "    free(role);\n");
        buf_puts(out, "}\n\n");
    }

    /* Phase 4 v1: Collect all defer thunks into a buffer so they can be
     * emitted after extern_decls and fwd_decls (defer bodies may call
     * extern-c functions or forward-declared Turmeric functions). */
    emit_pending_defer_thunks(&ctx, &defer_thunks);
    Buf concrete_struct_apps; buf_init(&concrete_struct_apps);
    type_codegen_emit_struct_apps(&concrete_struct_apps);
    Buf concrete_adt_apps; buf_init(&concrete_adt_apps);
    type_codegen_emit_adt_apps(&concrete_adt_apps);
    /* SYM1: interned runtime symbol records (struct __tur_sym + one per keyword).
     * Body emission above populated the registry via sym_codegen_register(). */
    Buf sym_records; buf_init(&sym_records);
    sym_codegen_emit(&sym_records, false);  /* single-file: static records */
    /* Phase E: fn-ptr typedefs for concrete fn fields in parametric structs */
    Buf concrete_fn_ptr_typedefs; buf_init(&concrete_fn_ptr_typedefs);
    type_codegen_emit_fn_ptr_typedefs(&concrete_fn_ptr_typedefs);

    /* Final assembly order (ensures correct C visibility):
     *  1. early_file  - struct typedefs + drop glue (visible to everything)
     *  2. concrete_adt_apps - monomorphized polymorphic ADT typedefs + ctor fns
     *  3. concrete_fn_ptr_typedefs - typed fn-ptr typedefs for parametric struct fields
     *  4. concrete_struct_apps - monomorphized generic struct typedefs
     *  5. extern_decls - user extern-c declarations
     *  6. fwd_decls   - Turmeric function forward declarations (visible to handlers)
     *  7. defer_thunks - defer body functions (may call extern-c or Turmeric fns)
     *  8. pending_handler_fns - effect handler functions (can call Turmeric fns)
     *  9. file        - Turmeric function definitions (can reference handler fns by name)
     * 10. main()      - entry point body
     */
    if (early_file.len)  { buf_write(out, early_file.data, early_file.len); buf_putc(out, '\n'); }
    if (sym_records.len) { buf_write(out, sym_records.data, sym_records.len); buf_putc(out, '\n'); }
    if (concrete_adt_apps.len) { buf_write(out, concrete_adt_apps.data, concrete_adt_apps.len); buf_putc(out, '\n'); }
    if (concrete_fn_ptr_typedefs.len) { buf_write(out, concrete_fn_ptr_typedefs.data, concrete_fn_ptr_typedefs.len); buf_putc(out, '\n'); }
    if (concrete_struct_apps.len) { buf_write(out, concrete_struct_apps.data, concrete_struct_apps.len); buf_putc(out, '\n'); }
    if (thunk_typedefs.len) { buf_write(out, thunk_typedefs.data, thunk_typedefs.len); buf_putc(out, '\n'); }
    if (extern_decls.len){ buf_write(out, extern_decls.data, extern_decls.len); buf_putc(out, '\n'); }
    if (fwd_decls.len)   { buf_write(out, fwd_decls.data, fwd_decls.len); buf_putc(out, '\n'); }
    if (defer_thunks.len){ buf_write(out, defer_thunks.data, defer_thunks.len); buf_putc(out, '\n'); }
    /* file-scope-c-block: top-level raw-C prelude -- after fwd_decls (so it may
     * call Turmeric functions) and before handler fns / file (so function defs
     * may reference the file-scope helpers it declares). */
    if (cprelude.len)    { buf_write(out, cprelude.data, cprelude.len); buf_putc(out, '\n'); }
    buf_free(&early_file);
    buf_free(&thunk_typedefs);
    buf_free(&extern_decls);
    buf_free(&fwd_decls);
    buf_free(&defer_thunks);
    buf_free(&cprelude);
    inline_c_dedup_free(&cprelude_dedup);
    buf_free(&concrete_struct_apps);
    buf_free(&concrete_adt_apps);
    buf_free(&concrete_fn_ptr_typedefs);
    buf_free(&sym_records);

    /* Phase 19: Effect handler functions (after fwd_decls so they can call
     * user-defined Turmeric functions, before file so fn defs can reference them). */
    if (ctx.pending_handler_fns && ctx.pending_handler_fns->len > 0) {
        buf_write(out, ctx.pending_handler_fns->data, ctx.pending_handler_fns->len);
        buf_free(ctx.pending_handler_fns);
        buf_init(ctx.pending_handler_fns);
    }

    if (file.len) { buf_write(out, file.data, file.len); buf_putc(out, '\n'); }

    if (!user_has_main) {
        /* Only generate main() if user didn't define one */
        buf_puts(out, "int main(int argc, char **argv) {\n");
        /* Phase R6: Set g_panic_trace from compiler flag */
        if (g_panic_trace) {
            buf_puts(out, "    g_panic_trace = 1;\n");
        }
        /* CLI-ARGS: Build *args* list from argv[1..] as a linked list of char* (as int64_t). */
        buf_puts(out, "    /* *args*: build cons list from argv[1..argc-1] */\n");
        buf_puts(out, "    g_tur_args = 0;\n");
        buf_puts(out, "    for (int _ai = argc - 1; _ai >= 1; _ai--) {\n");
        buf_puts(out, "        typedef struct { int64_t value; int64_t next; } __tur_args_cell;\n");
        buf_puts(out, "        __tur_args_cell *_c = (__tur_args_cell *)malloc(sizeof(__tur_args_cell));\n");
        buf_puts(out, "        _c->value = (int64_t)(intptr_t)argv[_ai];\n");
        buf_puts(out, "        _c->next = g_tur_args;\n");
        buf_puts(out, "        g_tur_args = (int64_t)(intptr_t)_c;\n");
        buf_puts(out, "    }\n");
        if (body.len) buf_write(out, body.data, body.len);
        buf_puts(out, "    return 0;\n");
        buf_puts(out, "}\n");
    }

    buf_free(&file);
    buf_free(&body);
    free(items);
    for (uint32_t i = 0; i < ctx.n_thunk_typedef_names; i++) free(ctx.thunk_typedef_names[i]);
    free(ctx.thunk_typedef_names);
    for (uint32_t i = 0; i < ctx.n_fatshim_names; i++) free(ctx.fatshim_names[i]);
    free(ctx.fatshim_names);
    for (uint32_t i = 0; i < ctx.n_poly_fatshim_names; i++) free(ctx.poly_fatshim_names[i]);
    free(ctx.poly_fatshim_names);
    free(ctx.env_struct_names);
    for (uint32_t i = 0; i < ctx.n_abi_specializations; i++) free(ctx.abi_specializations[i].clone_name);
    free(ctx.abi_specializations);
    free(ctx.specialized_call_exprs);
    /* specialized_call_names entries alias spec->clone_name; freed above. */
    free(ctx.specialized_call_names);
    free(ctx.carrier_call_bindings);
    arena_free(&type_arena);
    return 0;
}

/* ------------ Phase 2: Multi-file support ------------ */

/* Sanitize a module name for use in C header guards and the module's own
 * self-include ('/' -> "__", '-' -> '_'). */
static void sanitize_module_name(char *out, const char *name, size_t cap) {
    /* Must agree with mangle_module_name (emit_core.c): '/' -> "__" so the
     * header guard and the module's self-include line up with the mangled
     * names dependents use in their cross-module #includes and symbol
     * prefixes.  Single-segment names (no '/') are unaffected. */
    size_t k = 0;
    for (size_t i = 0; name[i] && k + 2 < cap; i++) {
        char c = name[i];
        if (c == '/') {
            out[k++] = '_';
            out[k++] = '_';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out[k++] = (c == '-') ? '_' : c;
        }
    }
    out[k] = '\0';
}

/* Mangle a module name for use as a C file base name / symbol prefix.
 * Uses double underscore for '/' (so geom/vector → geom__vector) and
 * single underscore for '-'. */
static void mangle_module_name(char *out, const char *name, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; name[i] && k < cap - 2; i++) {
        char c = name[i];
        if (c == '/') {
            out[k++] = '_';
            out[k++] = '_';
        } else if (c == '-') {
            out[k++] = '_';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_') {
            out[k++] = c;
        } else {
            out[k++] = '_';
        }
    }
    out[k] = '\0';
}

/* RP1: map a TypeKind to its Turmeric type-DSL spelling for the
 * exports.manifest. The dispatch table is intentionally narrow -- the
 * FFI runtime only handles primitives in v1; anything richer falls back
 * to :any so the dispatcher can still recognise the shape. */
static const char *manifest_type_tag(TypeKind k) {
    switch (k) {
        case TY_NIL:      return ":void";
        case TY_BOOL:     return ":bool";
        case TY_INT:      return ":int";
        case TY_FLOAT:    return ":float";
        case TY_CSTR:     return ":cstr";
        case TY_PTR_VOID: return ":ptr";
        case TY_INT8:     return ":int8";
        case TY_INT16:    return ":int16";
        case TY_INT32:    return ":int32";
        case TY_INT64:    return ":int64";
        case TY_UINT8:    return ":uint8";
        case TY_UINT16:   return ":uint16";
        case TY_UINT32:   return ":uint32";
        case TY_UINT64:   return ":uint64";
        case TY_FLOAT32:  return ":float32";
        case TY_FLOAT64:  return ":float64";
        case TY_NEVER:    return ":never";
        default:          return ":any";
    }
}

/* RP1: append a manifest line per exported defn. See emit.h for format. */
int emit_exports_manifest(Buf *out, const Expr *program) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "tur: emit_exports_manifest: expected EX_PROGRAM\n");
        return -1;
    }
    uint32_t n_items;
    const Expr **items = flatten_program_items(program, &n_items);
    for (uint32_t i = 0; i < n_items; i++) {
        const Expr *e = items[i];
        if (e->kind != EX_FN_DEF) continue;
        FnDef *fd = e->as.fn_def_.fn;
        const Binding *b = fd->binding;
        if (!b->is_exported) continue;
        /* `main` is exempt from module-prefixing and from --shared anyway. */
        if (b->name->len == 4 && memcmp(b->name->name, "main", 4) == 0) continue;
        if (e->type.kind != TY_FN) continue;
        /* Module qualifier: the defmodule name (or `_` for top-level
         * exports outside a defmodule -- those don't get a C prefix
         * either, so the host can still resolve them via the bare name). */
        const char *mod_name = b->defining_module_name
                             ? b->defining_module_name->name
                             : "_";
        char *mangled = raw_name_for_binding(b);
        buf_printf(out, "%s/%.*s -> %s :: (",
                   mod_name, (int)b->name->len, b->name->name, mangled);
        for (uint8_t j = 0; j < fd->n_params; j++) {
            if (j > 0) buf_puts(out, " ");
            buf_puts(out, manifest_type_tag(fd->param_types[j].kind));
        }
        if (e->type.as.fn.is_variadic) {
            if (fd->n_params > 0) buf_puts(out, " ");
            buf_printf(out, "& %s", manifest_type_tag(e->type.as.fn.rest_kind));
        }
        buf_puts(out, ") -> ");
        TypeKind ret = e->type.as.fn.result_kind;
        buf_puts(out, manifest_type_tag(ret));
        buf_putc(out, '\n');
        free(mangled);
    }
    free(items);
    return 0;
}

/* Emit a C header file for a module. Contains declarations (not definitions).
 * When separate_compilation is true (Phase M3): only exported functions are
 * declared, and #includes for each imported module's header are emitted. */
int emit_header(Buf *out, const char *module_name, const Expr *program,
                bool separate_compilation,
                const ForcedAbiSpec *forced, uint32_t n_forced) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "tur: emit_header: expected EX_PROGRAM\n");
        return -1;
    }

    char guard[256];
    sanitize_module_name(guard, module_name, sizeof(guard));

    /* Header guard */
    buf_printf(out, "/* generated by tur (phase 2) */\n");
    buf_printf(out, "#ifndef TUR_%s_H\n", guard);
    buf_printf(out, "#define TUR_%s_H\n\n", guard);

    /* Standard includes */
    buf_puts(out, "#include <stdint.h>\n");
    buf_puts(out, "#include <stdbool.h>\n");
    buf_puts(out, "#include <stdio.h>\n");
    buf_puts(out, "#include <stdlib.h>\n");
    buf_puts(out, "#include <string.h>\n\n");

    /* load-not-expanded-in-imported-or-project-modules: the whole-program path
     * emits the rank-2 polymorphic closure type `tur_poly_fn_t` as part of its
     * runtime preamble (emit_program), but the separate-compilation path does
     * not emit that preamble.  Exported signatures in this header -- and the
     * internal typeclass-method signatures in the matching .c (e.g. a spliced
     * `Functor` instance's `fmap`) -- can both reference it, so declare it once
     * here where every consumer (.c includers and importing modules) sees it. */
    if (separate_compilation) {
        /* Guarded so a module that includes several module headers (each of
         * which may declare this carrier) does not redefine the anonymous
         * struct -- which the C front-end reads as conflicting types. */
        buf_puts(out, "/* rank-2 polymorphic closure carrier (typeclass-method params) */\n");
        buf_puts(out, "#ifndef TUR_POLY_FN_T_DEFINED\n");
        buf_puts(out, "#define TUR_POLY_FN_T_DEFINED\n");
        buf_puts(out, "typedef struct { void *env; int64_t (*fn)(void *, int64_t); } tur_poly_fn_t;\n");
        buf_puts(out, "#endif\n\n");
    }

    /* SYM2 (runtime-symbols-plan): forward-declare the interned-symbol tag so
     * that exported signatures using `const struct __tur_sym *` (a :Sym param
     * or result) refer to a single file-scope tag.  Without this, a :Sym in
     * parameter position would declare the tag in prototype scope, conflicting
     * with the full definition emitted in the .c. */
    if (g_symbols_enabled) {
        buf_puts(out, "struct __tur_sym;  /* SYM2: interned runtime symbol */\n\n");
    }

    /* Phase M3: When separate compilation, emit #includes for imported modules. */
    if (separate_compilation) {
        for (uint32_t i = 0; i < program->as.program.n; i++) {
            const Expr *e = program->as.program.items[i];
            if (e->kind == EX_DEFMODULE) {
                const DefModule *mod = e->as.defmodule_.mod;
                for (uint32_t j = 0; j < mod->n_imports; j++) {
                    char imp_mangled[256];
                    mangle_module_name(imp_mangled, mod->imports[j].module_name->name,
                                       sizeof(imp_mangled));
                    buf_printf(out, "#include \"%s.h\"\n", imp_mangled);
                }
                if (mod->n_imports > 0) buf_putc(out, '\n');
            }
        }
    }

    type_codegen_reset_struct_apps();
    type_codegen_reset_adt_apps();
    type_codegen_reset_fn_ptr_typedefs();

    /* Forward declarations for functions.
     * In separate_compilation mode, only emit exported symbols. */
    uint32_t h_n_items;
    const Expr **h_items = flatten_program_items(program, &h_n_items);

    /* project-mode-defstruct-typedef-missing: emit `typedef struct Name {
     * fields... } Name;` into the header so both this module's .c and any
     * importing modules see the type.  Mirrors the Pass 0 in emit_program
     * (single-file mode) at the top of this file, minus the drop_glue/
     * walk_glue functions -- those stay `static` in the implementation file. */
    if (separate_compilation) {
        for (uint32_t i = 0; i < h_n_items; i++) {
            const Expr *e = h_items[i];
            if (e->kind != EX_DEF || !e->as.def_.struct_def) continue;
            StructDef *def = e->as.def_.struct_def;
            if (def->is_opaque) continue;
            /* Pre-register fn-ptr typedefs for concrete fn fields so they
             * land before the struct typedef that references them. */
            for (uint32_t j = 0; j < def->n_fields; j++) {
                StructField *f = &def->fields[j];
                if (f->kind == TY_FN && f->full_type && f->full_type->kind == TY_FN) {
                    const char *td = register_fn_ptr_typedef(f->full_type);
                    if (td) type_codegen_emit_fn_ptr_typedefs(out);
                }
            }
            buf_printf(out, "typedef struct %s {\n", def->name);
            for (uint32_t j = 0; j < def->n_fields; j++) {
                StructField *f = &def->fields[j];
                const char *ctype;
                if (f->kind == TY_FN && f->full_type && f->full_type->kind == TY_FN) {
                    const char *td = register_fn_ptr_typedef(f->full_type);
                    ctype = td ? td : "int64_t";
                } else switch (f->kind) {
                    case TY_INT:      ctype = "int64_t"; break;
                    case TY_BOOL:     ctype = "bool"; break;
                    case TY_FLOAT:    ctype = "double"; break;
                    case TY_CSTR:     ctype = "const char *"; break;
                    case TY_PTR_VOID: ctype = "void *"; break;
                    case TY_RC:
                    case TY_WEAK:     ctype = "RcControlBlock *"; break;
                    case TY_REF:
                    case TY_LREF:     ctype = "void *"; break;
                    case TY_INT8:     ctype = "int8_t"; break;
                    case TY_INT16:    ctype = "int16_t"; break;
                    case TY_INT32:    ctype = "int32_t"; break;
                    case TY_UINT8:    ctype = "uint8_t"; break;
                    case TY_UINT16:   ctype = "uint16_t"; break;
                    case TY_UINT32:   ctype = "uint32_t"; break;
                    case TY_UINT64:   ctype = "uint64_t"; break;
                    case TY_FLOAT32:  ctype = "float"; break;
                    default:          ctype = "int64_t"; break;
                }
                char *mfn = mangle_field_name(f->name);
                buf_printf(out, "    %s %s;\n", ctype, mfn);
                free(mfn);
            }
            buf_printf(out, "} %s;\n\n", def->name);
        }
    }

    for (uint32_t i = 0; i < h_n_items; i++) {
        const Expr *e = h_items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            bool is_main = (strcmp(fd->binding->name->name, "main") == 0);
            if (is_main) continue;
            if (separate_compilation && !fd->binding->is_exported) continue;
            if (e->type.kind == TY_FN) {
                if (e->type.as.fn.result_full_type) {
                    (void)type_c_name(*e->type.as.fn.result_full_type);
                } else {
                    (void)type_c_name(emit_type_from_kind(e->type.as.fn.result_kind));
                }
            }
            for (uint8_t j = 0; j < fd->n_params; j++) {
                if (e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[j]) {
                    (void)type_c_name(*e->type.as.fn.arg_full_types[j]);
                } else {
                    (void)type_c_name(fd->param_types[j]);
                }
            }
        } else if (e->kind == EX_EXTERN_C) {
            ExternC *ec = e->as.extern_c_.ext;
            (void)type_c_name(ec->return_type);
            for (uint8_t j = 0; j < ec->n_params; j++) (void)type_c_name(ec->param_types[j]);
        } else if (e->kind == EX_DEF && !e->as.def_.struct_def) {
            if (separate_compilation && e->as.def_.binding->is_exported) {
                (void)type_c_name(e->as.def_.binding->type);
            }
        }
    }
    type_codegen_emit_struct_apps(out);
    type_codegen_emit_adt_apps(out);
    type_codegen_emit_fn_ptr_typedefs(out);

    /* J4: In separate-compilation mode, emit extern declarations for any
     * ABI-specialization clones that this module owns (i.e. the generic FnDef
     * is defined here).  Importing modules include this header and thereby pick
     * up the decls without needing their own extern bookkeeping.
     * J5/J6: Also emit decls for forced specs from the cross-module cache. */
    if (separate_compilation) {
        EmitCtx hdr_ctx;
        memset(&hdr_ctx, 0, sizeof(hdr_ctx));
        hdr_ctx.separate_compilation = true;
        /* ASan/LSan plan (Option C): arena for transient ABI-spec Type scratch,
         * freed in bulk before this block returns. */
        Arena hdr_type_arena; arena_init(&hdr_type_arena, 0);
        hdr_ctx.type_arena = &hdr_type_arena;
        emit_abi_scan_program(&hdr_ctx, h_items, h_n_items);

        /* J6: Inject forced specs (borrow specs from other modules pointing here). */
        for (uint32_t fi = 0; fi < n_forced; fi++) {
            const ForcedAbiSpec *fs = &forced[fi];
            /* Check if we already have this clone from local call sites. */
            bool already = false;
            for (uint32_t si = 0; si < hdr_ctx.n_abi_specializations; si++) {
                if (strcmp(hdr_ctx.abi_specializations[si].clone_name, fs->clone_name) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) continue;
            /* Find the binding and fn_expr in this module's items. */
            Binding *b = NULL;
            const Expr *fn_expr = NULL;
            for (uint32_t ii = 0; ii < h_n_items; ii++) {
                if (h_items[ii]->kind != EX_FN_DEF) continue;
                FnDef *fd2 = h_items[ii]->as.fn_def_.fn;
                if (!fd2 || !fd2->binding || !fd2->binding->name) continue;
                if (strcmp(fd2->binding->name->name, fs->fn_symbol) == 0) {
                    b = fd2->binding;
                    fn_expr = h_items[ii];
                    break;
                }
            }
            if (!b || !fn_expr || !fn_expr->as.fn_def_.fn) continue;
            /* Build arg_types[] and result_type from TypeKind values. */
            Type arg_types[16];
            for (uint8_t ai = 0; ai < fs->n_args; ai++)
                arg_types[ai] = emit_type_from_kind(fs->arg_kinds[ai]);
            Type result_type = emit_type_from_kind(fs->result_kind);
            /* Grow hdr_ctx.abi_specializations and add spec. */
            if (hdr_ctx.n_abi_specializations >= hdr_ctx.cap_abi_specializations) {
                uint32_t nc = hdr_ctx.cap_abi_specializations ? hdr_ctx.cap_abi_specializations * 2 : 4;
                EmitAbiSpecialization *ns = (EmitAbiSpecialization *)realloc(
                    hdr_ctx.abi_specializations, nc * sizeof(EmitAbiSpecialization));
                if (!ns) { fprintf(stderr, "tur: oom\n"); abort(); }
                hdr_ctx.abi_specializations = ns;
                hdr_ctx.cap_abi_specializations = nc;
            }
            EmitAbiSpecialization *sp = &hdr_ctx.abi_specializations[hdr_ctx.n_abi_specializations++];
            memset(sp, 0, sizeof(*sp));
            sp->fn_expr = fn_expr;
            sp->fn = fn_expr->as.fn_def_.fn;
            sp->binding = b;
            sp->n_args = fs->n_args;
            sp->result_type = result_type;
            for (uint8_t ai = 0; ai < fs->n_args; ai++) sp->arg_types[ai] = arg_types[ai];
            sp->clone_name = strdup(fs->clone_name);
            sp->external_linkage = true;
        }

        uint32_t n_decls = 0;
        for (uint32_t i = 0; i < hdr_ctx.n_abi_specializations; i++) {
            const EmitAbiSpecialization *spec = &hdr_ctx.abi_specializations[i];
            if (!spec->fn) continue; /* skip borrow specs */
            buf_puts(out, type_c_name(spec->result_type));
            buf_printf(out, " %s(", spec->clone_name);
            for (uint8_t j = 0; j < spec->n_args; j++) {
                if (j > 0) buf_puts(out, ", ");
                if (spec->fn->params[j]->is_poly_fn) {
                    buf_puts(out, "tur_poly_fn_t");
                } else if (spec->fn->param_types[j].kind == TY_FN) {
                    buf_puts(out, "int64_t");
                } else {
                    buf_puts(out, type_c_name(spec->arg_types[j]));
                }
            }
            buf_puts(out, ");\n");
            n_decls++;
        }
        /* Free all clone_names (both owned and borrow specs). */
        for (uint32_t i = 0; i < hdr_ctx.n_abi_specializations; i++)
            free(hdr_ctx.abi_specializations[i].clone_name);
        free(hdr_ctx.abi_specializations);
        free(hdr_ctx.specialized_call_exprs);
        free(hdr_ctx.specialized_call_names);
        free(hdr_ctx.carrier_call_bindings);
        arena_free(&hdr_type_arena);
        if (n_decls > 0) buf_putc(out, '\n');
    }

    for (uint32_t i = 0; i < h_n_items; i++) {
        const Expr *e = h_items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            const char *fn_name = raw_name_for_binding(fd->binding);
            bool is_main = (strcmp(fd->binding->name->name, "main") == 0);

            if (is_main) { free((void*)fn_name); continue; }

            /* In separate_compilation mode, only declare exported symbols. */
            if (separate_compilation && !fd->binding->is_exported) {
                free((void*)fn_name); continue;
            }

            /* Emit function declaration */
            if (e->type.kind == TY_FN) {
                if (e->type.as.fn.result_full_type) {
                    buf_puts(out, type_c_name(*e->type.as.fn.result_full_type));
                } else {
                    TypeKind result = e->type.as.fn.result_kind;
                    buf_puts(out, type_c_name(emit_type_from_kind(result)));
                }
            } else {
                buf_puts(out, "void");
            }
            buf_printf(out, " %s(", fn_name);
            for (uint8_t j = 0; j < fd->n_params; j++) {
                if (j > 0) buf_puts(out, ", ");
                /* header-fat-param-emitted-as-inner-type.md: mirror the
                 * forward-decl carrier logic from emit_implementation so the
                 * header prototype agrees with the .c definition. ^fat params
                 * are always the int64_t carrier in the prototype; TY_FN
                 * params are also int64_t; poly-fn params use the poly carrier. */
                if (fd->params[j]->is_poly_fn) {
                    buf_puts(out, "tur_poly_fn_t");
                } else if (fd->param_types[j].kind == TY_FN) {
                    buf_puts(out, "int64_t");
                } else if (fd->params[j]->is_fat) {
                    buf_puts(out, "int64_t");
                } else {
                    /* Phase D: mirror emit_fn_def's pass-by-ptr logic. */
                    bool _hdr_inline_c = (fd->body && fd->body->kind == EX_INLINE_C);
                    Type _hdr_pty = (e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[j])
                        ? *e->type.as.fn.arg_full_types[j]
                        : fd->param_types[j];
                    if (!fd->closure && !_hdr_inline_c && type_struct_pass_by_ptr(_hdr_pty)) {
                        buf_printf(out, "const %s *", type_c_name(_hdr_pty));
                    } else {
                        buf_puts(out, type_c_name(_hdr_pty));
                    }
                }
            }
            buf_puts(out, ");\n");
            free((void*)fn_name);
        } else if (e->kind == EX_EXTERN_C) {
            ExternC *ec = e->as.extern_c_.ext;
            char *ec_mangled = mangle_field_name(ec->c_name->name); /* legacy fold */
            buf_printf(out, "extern %s %s(",
                       type_c_name(ec->return_type),
                       ec_mangled);
            free(ec_mangled);
            for (uint8_t j = 0; j < ec->n_params; j++) {
                if (j > 0) buf_puts(out, ", ");
                buf_puts(out, type_c_name(ec->param_types[j]));
            }
            buf_puts(out, ");\n");
        } else if (e->kind == EX_DEF && !e->as.def_.struct_def) {
            /* Phase M6: exported global variables need extern declarations
             * in the header so other modules can reference them. */
            if (separate_compilation && e->as.def_.binding->is_exported) {
                const char *var_name = raw_name_for_binding(e->as.def_.binding);
                buf_printf(out, "extern %s %s;\n",
                           type_c_name(e->as.def_.binding->type), var_name);
                free((void*)var_name);
            }
        }
    }
    free(h_items);

    if (out->len > 0 && out->data[out->len - 1] != '\n') {
        buf_putc(out, '\n');
    }

    /* Header guard end */
    buf_printf(out, "#endif /* TUR_%s_H */\n", guard);
    return 0;
}

/* Emit a C implementation file for a module. Contains definitions. */
/* Emit a C implementation file for a module. Contains definitions.
 * When separate_compilation is true (Phase M3): #includes imported modules'
 * headers instead of emitting their code inline. */
int emit_implementation(Buf *out, const char *module_name, const Expr *program,
                        bool separate_compilation,
                        const ForcedAbiSpec *forced, uint32_t n_forced,
                        BorrowSpecInfo **out_borrow_specs, uint32_t *out_n_borrow_specs) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "tur: emit_implementation: expected EX_PROGRAM\n");
        return -1;
    }

    type_codegen_reset_struct_apps();
    type_codegen_reset_adt_apps();
    type_codegen_reset_fn_ptr_typedefs();
    sym_codegen_reset();   /* SYM1/SYM2: clear interned-symbol records for this TU */

    Buf file; buf_init(&file);
    Buf body; buf_init(&body);

    Buf thunk_typedefs2; buf_init(&thunk_typedefs2);

    EmitCtx ctx;
    ctx.file = &file;
    ctx.main_ = &body;
    ctx.program_root = program;   /* cps-transform-plan (a): serial env instance scan */
    ctx.thunk_typedefs = &thunk_typedefs2;
    ctx.indent = 4;
    ctx.tmp_n = 0;
    ctx.fn_params = NULL;
    ctx.n_fn_params = 0;
    /* Phase 3: closure tracking */
    ctx.closure = NULL;
    ctx.env_var_name = NULL;
    /* Phase 3: env struct tracking */
    ctx.env_struct_names = NULL;
    ctx.n_env_struct_names = 0;
    ctx.cap_env_struct_names = 0;
    ctx.thunk_typedef_names = NULL;
    ctx.n_thunk_typedef_names = 0;
    ctx.cap_thunk_typedef_names = 0;
    ctx.fatshim_names = NULL;
    ctx.n_fatshim_names = 0;
    ctx.cap_fatshim_names = 0;
    ctx.poly_fatshim_names = NULL;
    ctx.n_poly_fatshim_names = 0;
    ctx.cap_poly_fatshim_names = 0;
    /* Phase 3/4: Track return emission */
    ctx.return_emitted = false;
    /* Phase 19: Pending effect handler function buffer */
    Buf pending_hfns2; buf_init(&pending_hfns2);
    ctx.pending_handler_fns = &pending_hfns2;
    /* Phase R5: no-unwind context (false at top level; set per-function) */
    ctx.no_unwind = false;
    /* Phase M3: separate compilation mode */
    ctx.separate_compilation = separate_compilation;
    /* Phase 19D: handle captures (NULL at top level) */
    ctx.handle_captures = NULL;
    ctx.n_handle_captures = 0;
    ctx.handle_env_name = NULL;
    /* GF1: generator struct context (NULL outside a _next function) */
    ctx.gen_struct_bindings = NULL;
    ctx.n_gen_struct_bindings = 0;
    ctx.gen_var_name = NULL;
    ctx.gen_struct_type = NULL;
    ctx.gen_hdr_emitted = false;
    ctx.abi_specializations = NULL;
    ctx.n_abi_specializations = 0;
    ctx.cap_abi_specializations = 0;
    ctx.specialized_call_exprs = NULL;
    ctx.specialized_call_names = NULL;
    ctx.n_specialized_calls = 0;
    ctx.cap_specialized_calls = 0;
    ctx.carrier_call_bindings = NULL;
    ctx.n_carrier_call_bindings = 0;
    ctx.cap_carrier_call_bindings = 0;
    ctx.current_abi_specialization = NULL;
    ctx.current_scan_fn = NULL;
    ctx.fn_name_override = NULL;
    ctx.fn_name_override_external = false;  /* J3 */
    ctx.n_pbp_params = 0;    /* Phase D: no pbp params at top level */
    /* Phase 4 v1: frame/defer tracking (not initialized above; zero them here). */
    ctx.frame_var = NULL;
    ctx.in_scope_with_defers = false;
    ctx.pending_defer_thunks = NULL;
    ctx.defer_captures = NULL;
    ctx.n_defer_captures = 0;
    /* ASan/LSan plan (Option C): arena for transient ABI-spec Type scratch,
     * freed in bulk at the end of this function. */
    Arena type_arena2; arena_init(&type_arena2, 0);
    ctx.type_arena = &type_arena2;

    char guard[256];
    sanitize_module_name(guard, module_name, sizeof(guard));

    /* Include the corresponding header (which already pulls in imported headers
     * when separate_compilation is true). */
    buf_printf(out, "/* generated by tur (phase 2) */\n");
    buf_printf(out, "#include \"%s.h\"\n\n", guard);
    /* prelude-macros (Defect B / F3): inject the `cons` cons-cell helper into
     * this module's .c when it references cons.  emit_header emits only a
     * minimal preamble, so project-mode TUs get the helper here. */
    emit_cons_helper(out);

    uint32_t impl_n_items;
    const Expr **impl_items = flatten_program_items(program, &impl_n_items);

    /* J1/J2: ABI specialization scan (populates ctx.abi_specializations). */
    emit_abi_scan_program(&ctx, impl_items, impl_n_items);

    /* J2: Phase I parity -- emit ABI trace for the impl path. */
    if (g_emit_abi_trace) {
        for (uint32_t i = 0; i < impl_n_items; i++) {
            emit_abi_trace_expr(&ctx, impl_items[i]);
        }
    }

    /* J3/J4: In separate-compilation mode, every spec whose FnDef lives in
     * this module (fn_expr != NULL) is an owned spec: emit with external
     * linkage so other TUs can link to it.  Borrow specs (fn_expr == NULL)
     * just need the call-site rewrite; their body comes from the owner. */
    if (separate_compilation) {
        for (uint32_t i = 0; i < ctx.n_abi_specializations; i++) {
            if (ctx.abi_specializations[i].fn_expr != NULL)
                ctx.abi_specializations[i].external_linkage = true;
        }
    }

    /* J6: Inject forced specs -- clones that borrower modules need and that
     * this module must own (emit body + extern decl) even without local call
     * sites at the concrete type. */
    for (uint32_t fi = 0; fi < n_forced; fi++) {
        const ForcedAbiSpec *fs = &forced[fi];
        /* Dedup: skip if already found via a local call site. */
        bool already = false;
        for (uint32_t si = 0; si < ctx.n_abi_specializations; si++) {
            if (strcmp(ctx.abi_specializations[si].clone_name, fs->clone_name) == 0) {
                already = true;
                break;
            }
        }
        if (already) continue;
        /* Find binding and fn_expr in this module's items. */
        Binding *b = NULL;
        const Expr *fn_expr2 = NULL;
        for (uint32_t ii = 0; ii < impl_n_items; ii++) {
            if (impl_items[ii]->kind != EX_FN_DEF) continue;
            FnDef *fd2 = impl_items[ii]->as.fn_def_.fn;
            if (!fd2 || !fd2->binding || !fd2->binding->name) continue;
            if (strcmp(fd2->binding->name->name, fs->fn_symbol) == 0) {
                b = fd2->binding;
                fn_expr2 = impl_items[ii];
                break;
            }
        }
        if (!b || !fn_expr2 || !fn_expr2->as.fn_def_.fn) continue;
        FnDef *fd2 = fn_expr2->as.fn_def_.fn;
        if (fd2->closure || !fd2->body) continue;
        /* Build spec. */
        if (ctx.n_abi_specializations >= ctx.cap_abi_specializations) {
            uint32_t nc = ctx.cap_abi_specializations ? ctx.cap_abi_specializations * 2 : 4;
            EmitAbiSpecialization *ns = (EmitAbiSpecialization *)realloc(
                ctx.abi_specializations, nc * sizeof(EmitAbiSpecialization));
            if (!ns) { fprintf(stderr, "tur: oom\n"); abort(); }
            ctx.abi_specializations = ns;
            ctx.cap_abi_specializations = nc;
        }
        EmitAbiSpecialization *sp = &ctx.abi_specializations[ctx.n_abi_specializations++];
        memset(sp, 0, sizeof(*sp));
        sp->fn_expr = fn_expr2;
        sp->fn = fd2;
        sp->binding = b;
        sp->n_args = fs->n_args;
        sp->result_type = emit_type_from_kind(fs->result_kind);
        for (uint8_t ai = 0; ai < fs->n_args; ai++)
            sp->arg_types[ai] = emit_type_from_kind(fs->arg_kinds[ai]);
        sp->clone_name = strdup(fs->clone_name);
        sp->external_linkage = true;
        /* No call-site rewrite needed (no local call site). */
    }

    /* J6: Collect borrow specs for output (so cmd_build_multi can determine
     * which owner modules need forced recompilation). */
    if (out_borrow_specs && out_n_borrow_specs) {
        uint32_t nb = 0;
        for (uint32_t i = 0; i < ctx.n_abi_specializations; i++) {
            if (!ctx.abi_specializations[i].fn_expr) nb++;
        }
        if (nb > 0) {
            BorrowSpecInfo *bs = (BorrowSpecInfo *)malloc(nb * sizeof(BorrowSpecInfo));
            if (!bs) { fprintf(stderr, "tur: oom\n"); abort(); }
            uint32_t bi = 0;
            for (uint32_t i = 0; i < ctx.n_abi_specializations; i++) {
                const EmitAbiSpecialization *spec = &ctx.abi_specializations[i];
                if (spec->fn_expr) continue;
                BorrowSpecInfo *bsi = &bs[bi++];
                bsi->clone_name = strdup(spec->clone_name);
                /* strdup owning_module and fn_symbol: the arena is freed when
                 * compile_to_implementation returns, so the Symbol* pointers
                 * would become dangling without a copy. */
                bsi->owning_module = (spec->binding && spec->binding->defining_module_name)
                    ? strdup(spec->binding->defining_module_name->name) : NULL;
                bsi->fn_symbol = (spec->binding && spec->binding->name)
                    ? strdup(spec->binding->name->name) : NULL;
                bsi->result_kind = spec->result_type.kind;
                bsi->n_args = spec->n_args;
                for (uint8_t ai = 0; ai < spec->n_args; ai++)
                    bsi->arg_kinds[ai] = spec->arg_types[ai].kind;
            }
            *out_borrow_specs = bs;
            *out_n_borrow_specs = nb;
        } else {
            *out_borrow_specs = NULL;
            *out_n_borrow_specs = 0;
        }
    }

    /* J2: Clone forward declarations (emitted before function definitions). */
    Buf impl_fwd_decls; buf_init(&impl_fwd_decls);
    for (uint32_t i = 0; i < ctx.n_abi_specializations; i++) {
        emit_abi_forward_decl(&impl_fwd_decls, &ctx.abi_specializations[i]);
    }
    /* Forward declarations for module-local functions so that mutually-recursive
     * static C functions resolve at C-compile time (parity with emit_program). */
    emit_fn_forward_decls(&ctx, &impl_fwd_decls, impl_items, impl_n_items);

    /* Check if user defined a main function */
    bool user_has_main = false;
    for (uint32_t i = 0; i < impl_n_items; i++) {
        const Expr *e = impl_items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            if (strcmp(fd->binding->name->name, "main") == 0) {
                user_has_main = true;
                break;
            }
        }
    }

    /* CLI-ARGS: In separate compilation mode, the standard preamble (which
     * declares g_tur_args) is NOT included.  If this module defines main(),
     * emit_fns.c will reference g_tur_args inside the function body, so we
     * must declare it here. */
    if (separate_compilation && user_has_main) {
        buf_puts(out, "static int64_t g_tur_args = 0;  /* *args*: CLI arguments as list of :cstr */\n\n");
    }

    /* Phase M5: collect top-level EX_DEFER nodes for atexit registration. */
    uint32_t n_module_defers = 0;
    for (uint32_t i = 0; i < impl_n_items; i++) {
        if (impl_items[i]->kind == EX_DEFER) n_module_defers++;
    }
    const Expr **module_defers = NULL;
    if (n_module_defers > 0) {
        module_defers = (const Expr **)malloc(n_module_defers * sizeof(Expr *));
        uint32_t di = 0;
        for (uint32_t i = 0; i < impl_n_items; i++) {
            if (impl_items[i]->kind == EX_DEFER)
                module_defers[di++] = impl_items[i];
        }
    }

    /* Pass 0: emit base ADT typedefs + constructor functions for every
     * defdata/defgadt in this module.  The header (emit_header) never emits the
     * base `tur_adt_<Name>` typedef -- only monomorphized type-applications --
     * so an ADT used internally (e.g. one spliced in by a top-level
     * (load "stdlib/either.tur")) would otherwise reference `tur_adt_Either` /
     * `ctor_Left` with no definition.  Mirrors emit_program's Pass 0 (which
     * routes through the same helper) but lands in the per-module .c.  Emitted
     * into a dedicated buffer so it precedes the forward decls and bodies that
     * reference these types in the final assembly.  Struct typedefs are NOT
     * emitted here -- emit_header already emits every struct typedef into the
     * header this .c #includes.  See
     * docs/reported/load-not-expanded-in-imported-or-project-modules.md. */
    Buf impl_early; buf_init(&impl_early);
    for (uint32_t i = 0; i < impl_n_items; i++) {
        const Expr *e = impl_items[i];
        if (e->kind == EX_DEFDATA || e->kind == EX_DEFGADT) {
            const AdtDef *adef = (e->kind == EX_DEFGADT)
                ? e->as.defgadt_.def : e->as.defdata_.def;
            emit_adt_typedef_and_ctors(&impl_early, adef);
        }
    }

    /* Pass 1a: emit all file-scope inline-C blocks before any defn body.
     * Dependency-based reordering (or a top-level C block that lands after
     * its defmodule in the flat array) can otherwise place a defn body before
     * the typedefs/helpers it needs from the C block.  Collecting all C blocks
     * first mirrors what emit_program does via the dedicated cprelude buffer.
     * file-scope-inline-c-dedup: skip byte-identical repeats so the shared
     * "redeclare the carrier struct per module" idiom does not produce a C
     * `redefinition of 'struct __foo'` when several modules land in one TU. */
    InlineCDedup impl_dedup = {0};
    for (uint32_t i = 0; i < impl_n_items; i++) {
        const Expr *e = impl_items[i];
        if (e->kind != EX_INLINE_C) continue;
        InlineC *ic = e->as.inline_c_.inline_c;
        if (ic && ic->code.p && ic->code.len > 0 &&
            !inline_c_dedup_seen(&impl_dedup, ic->code.p, ic->code.len)) {
            buf_write(&file, ic->code.p, ic->code.len);
            buf_putc(&file, '\n');
        }
    }
    inline_c_dedup_free(&impl_dedup);

    /* Pass 1b: emit all top-level definitions (EX_INLINE_C already handled). */
    for (uint32_t i = 0; i < impl_n_items; i++) {
        const Expr *e = impl_items[i];
        if (e->kind == EX_DEFER) {
            /* Phase M5: module-level defers are handled after this pass. */
            continue;
        } else if (e->kind == EX_DEF) {
            /* project-mode-defstruct-typedef-missing: a struct-def EX_DEF
             * represents a type declaration, not a runtime value.  The
             * typedef itself is emitted into the header (emit_header);
             * skip the bogus `Name Name_N;` variable declaration here. */
            if (e->as.def_.struct_def) continue;
            char *bn = name_for_binding(&ctx, e->as.def_.binding);
            /* Phase M6: exported def bindings get extern linkage in separate
             * compilation mode so other modules can reference them. */
            bool def_needs_static = !(separate_compilation &&
                                      e->as.def_.binding->is_exported);
            buf_printf(&file, "%s%s %s;\n",
                       def_needs_static ? "static " : "",
                       type_c_name(e->as.def_.binding->type), bn);
            if (e->as.def_.init) {
                char *iv = emit_value(&ctx, &body, e->as.def_.init);
                indent_buf(&body, ctx.indent);
                buf_printf(&body, "%s = %s;\n", bn, iv);
                free(iv);
            }
            free(bn);
        } else if (e->kind == EX_FN_DEF) {
            /* Emit function definition */
            emit_fn_def(&ctx, &file, e);
        } else if (e->kind == EX_EXTERN_C) {
            /* In implementation, just emit the extern declaration reference */
            ExternC *ec = e->as.extern_c_.ext;
            char *ec_mangled = mangle_field_name(ec->c_name->name); /* legacy fold */
            buf_printf(&file, "extern %s %s(",
                       type_c_name(ec->return_type),
                       ec_mangled);
            free(ec_mangled);
            for (uint8_t j = 0; j < ec->n_params; j++) {
                if (j > 0) buf_puts(&file, ", ");
                buf_puts(&file, type_c_name(ec->param_types[j]));
            }
            buf_puts(&file, ");\n");
        } else if (e->kind == EX_INLINE_C) {
            /* Already emitted in Pass 1a above. */
            continue;
        } else {
            emit_stmt(&ctx, &body, e);
        }
    }
    free(impl_items);

    /* J2: Emit clone bodies for owned specs (borrow specs have fn==NULL; skip
     * them -- the owner module's TU provides the definition). */
    for (uint32_t i = 0; i < ctx.n_abi_specializations; i++) {
        if (!ctx.abi_specializations[i].fn_expr) continue;
        ctx.current_abi_specialization = &ctx.abi_specializations[i];
        ctx.fn_name_override = ctx.abi_specializations[i].clone_name;
        ctx.fn_name_override_external = ctx.abi_specializations[i].external_linkage;
        emit_fn_def(&ctx, &file, ctx.abi_specializations[i].fn_expr);
        ctx.fn_name_override = NULL;
        ctx.fn_name_override_external = false;
        ctx.current_abi_specialization = NULL;
        ctx.current_scan_fn = NULL;
    }

    /* Phase M5: emit module-level defer thunks + atexit constructor. */
    if (n_module_defers > 0) {
        buf_puts(&file, "\n/* Phase M5: module-level defers */\n");
        /* Emit one static thunk per deferred expression. */
        for (uint32_t i = 0; i < n_module_defers; i++) {
            buf_printf(&file, "static void __module_defer_%u(void) {\n", i);
            Buf thunk_body; buf_init(&thunk_body);
            const char *saved_frame = ctx.frame_var;
            ctx.frame_var = NULL;
            ctx.indent = 4;
            emit_stmt(&ctx, &thunk_body, module_defers[i]->as.defer_.body);
            ctx.frame_var = saved_frame;
            buf_write(&file, thunk_body.data, thunk_body.len);
            buf_free(&thunk_body);
            buf_puts(&file, "}\n");
        }
        /* Emit constructor that registers thunks via atexit in definition
         * order. atexit is LIFO so last-defined defer fires first, matching
         * function-level defer semantics. */
        buf_printf(&file,
            "__attribute__((constructor))\n"
            "static void __module_defers_%s_init(void) {\n", guard);
        for (uint32_t i = 0; i < n_module_defers; i++) {
            buf_printf(&file, "    atexit(__module_defer_%u);\n", i);
        }
        buf_puts(&file, "}\n");
        free(module_defers);
    }

    /* Assemble: includes + file-scope decls + body (initializers).
     * In separate_compilation (M3) mode, never emit an auto-generated main():
     * each module is compiled independently and the user is responsible for
     * providing exactly one explicit main() across all modules. */
    /* SYM2: interned-symbol records for this TU.  Function bodies (in `file`)
     * were emitted above and registered their keywords; emit the struct def +
     * records now, before the function definitions that reference them.  Weak
     * external linkage lets the linker fold same-named records across TUs. */
    Buf sym_records2; buf_init(&sym_records2);
    sym_codegen_emit(&sym_records2, separate_compilation);
    if (sym_records2.len) { buf_write(out, sym_records2.data, sym_records2.len); buf_putc(out, '\n'); }
    buf_free(&sym_records2);
    /* load-not-expanded-in-imported-or-project-modules: base ADT typedefs +
     * ctors (Pass 0 above), then the on-demand type-application + fn-ptr
     * typedefs registered while emitting the bodies (e.g. `tur_fnptr_*` carriers
     * returned by typeclass-method impls).  These must precede the forward decls
     * and function definitions that reference them.  Mirrors the whole-program
     * assembly order in emit_program (early_file, adt_apps, fn_ptr_typedefs). */
    if (impl_early.len) { buf_write(out, impl_early.data, impl_early.len); buf_putc(out, '\n'); }
    buf_free(&impl_early);
    /* fn-ptr typedefs (e.g. `tur_fnptr_int64_t_int64_t_t`) registered while
     * emitting the bodies.  A typedef name that also appears in an exported
     * signature is emitted into the header too, but an identical fn-ptr typedef
     * redefinition is well-formed (same type), so this stays conflict-free.
     * Monomorphized ADT-application structs are deliberately NOT re-flushed here:
     * those carry anonymous-struct payloads the header already emits for exported
     * uses, and re-emitting one would be a struct redefinition. */
    Buf impl_fn_ptr_typedefs; buf_init(&impl_fn_ptr_typedefs);
    type_codegen_emit_fn_ptr_typedefs(&impl_fn_ptr_typedefs);
    if (impl_fn_ptr_typedefs.len) { buf_write(out, impl_fn_ptr_typedefs.data, impl_fn_ptr_typedefs.len); buf_putc(out, '\n'); }
    buf_free(&impl_fn_ptr_typedefs);
    if (thunk_typedefs2.len) { buf_write(out, thunk_typedefs2.data, thunk_typedefs2.len); buf_putc(out, '\n'); }
    /* J2: Clone forward decls precede function definitions. */
    if (impl_fwd_decls.len) { buf_write(out, impl_fwd_decls.data, impl_fwd_decls.len); buf_putc(out, '\n'); }
    if (file.len) { buf_write(out, file.data, file.len); buf_putc(out, '\n'); }
    if (!separate_compilation && !user_has_main) {
        /* Only generate main() if user didn't define one (single-file mode) */
        buf_puts(out, "int main(int argc, char **argv) {\n");
        /* Phase R6: Set g_panic_trace from compiler flag */
        if (g_panic_trace) {
            buf_puts(out, "    g_panic_trace = 1;\n");
        }
        /* CLI-ARGS: Build *args* list from argv[1..] as a linked list of char* (as int64_t). */
        buf_puts(out, "    /* *args*: build cons list from argv[1..argc-1] */\n");
        buf_puts(out, "    g_tur_args = 0;\n");
        buf_puts(out, "    for (int _ai = argc - 1; _ai >= 1; _ai--) {\n");
        buf_puts(out, "        typedef struct { int64_t value; int64_t next; } __tur_args_cell;\n");
        buf_puts(out, "        __tur_args_cell *_c = (__tur_args_cell *)malloc(sizeof(__tur_args_cell));\n");
        buf_puts(out, "        _c->value = (int64_t)(intptr_t)argv[_ai];\n");
        buf_puts(out, "        _c->next = g_tur_args;\n");
        buf_puts(out, "        g_tur_args = (int64_t)(intptr_t)_c;\n");
        buf_puts(out, "    }\n");
        if (body.len) buf_write(out, body.data, body.len);
        buf_puts(out, "    return 0;\n");
        buf_puts(out, "}\n");
    }

    buf_free(&file);
    buf_free(&body);
    buf_free(&impl_fwd_decls);
    buf_free(&thunk_typedefs2);
    for (uint32_t i = 0; i < ctx.n_thunk_typedef_names; i++) free(ctx.thunk_typedef_names[i]);
    free(ctx.thunk_typedef_names);
    for (uint32_t i = 0; i < ctx.n_fatshim_names; i++) free(ctx.fatshim_names[i]);
    free(ctx.fatshim_names);
    for (uint32_t i = 0; i < ctx.n_poly_fatshim_names; i++) free(ctx.poly_fatshim_names[i]);
    free(ctx.poly_fatshim_names);
    free(ctx.env_struct_names);
    for (uint32_t i = 0; i < ctx.n_abi_specializations; i++) free(ctx.abi_specializations[i].clone_name);
    free(ctx.abi_specializations);
    free(ctx.specialized_call_exprs);
    /* specialized_call_names entries alias spec->clone_name; freed above. */
    free(ctx.specialized_call_names);
    free(ctx.carrier_call_bindings);
    arena_free(&type_arena2);
    return 0;
}
