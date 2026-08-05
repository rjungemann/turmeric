/* elab_unsafe.c -- unsafe pointer ops, casts, raw memory, and C FFI primitives. */
#include "elab_internal.h"

/* Phase U3: Unsafe primitives implementations */

Expr *elab_ptr_deref(Elab *e, const Form *call) {
    /* (ptr-deref ptr) - dereference a pointer, return the value at that address */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-deref requires exactly 1 argument: (ptr-deref ptr)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-deref requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-deref requires ptr<void>, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_deref, ptr->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-deref: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = ptr;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_ptr_write(Elab *e, const Form *call) {
    /* (ptr-write ptr value) - write a value to a pointer address */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-write requires exactly 2 arguments: (ptr-write ptr value)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-write requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-write requires ptr<void> as first argument, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_write, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-write: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_ptr_add(Elab *e, const Form *call) {
    /* (ptr-add ptr offset) - pointer arithmetic: add offset to pointer */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires exactly 2 arguments: (ptr-add ptr offset)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *offset = elab_form(e, call->as.list.items[2]);
    if (!offset) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires ptr<void> as first argument, got %s", type_name(ptr->type));
        return NULL;
    }
    if (offset->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires int as second argument, got %s", type_name(offset->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_add, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-add: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = offset;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_ptr_sub(Elab *e, const Form *call) {
    /* (ptr-sub ptr offset) - pointer arithmetic: subtract offset from pointer */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires exactly 2 arguments: (ptr-sub ptr offset)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *offset = elab_form(e, call->as.list.items[2]);
    if (!offset) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires ptr<void> as first argument, got %s", type_name(ptr->type));
        return NULL;
    }
    if (offset->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires int as second argument, got %s", type_name(offset->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_sub, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-sub: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = offset;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_ptr_nullq(Elab *e, const Form *call) {
    /* (ptr-null? ptr) - check if pointer is null */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-null? requires exactly 1 argument: (ptr-null? ptr)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-null? requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-null? requires ptr<void>, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_nullq, ptr->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-null?: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = ptr;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_ptr_of(Elab *e, const Form *call) {
    /* (ptr-of value) - get pointer to a value (address-of) */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-of requires exactly 1 argument: (ptr-of value)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-of requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_of, value->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-of: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_unsafe_cast(Elab *e, const Form *call) {
    /* (unsafe-cast value to-type) - C-style cast
     * For v1, we just check we're in an unsafe block and emit a cast to int64_t
     * This is a simplified implementation; full type checking comes later */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "unsafe-cast requires exactly 2 arguments: (unsafe-cast value to-type)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "unsafe-cast requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;
    /* For v1, we ignore the target type and just cast to int64_t */
    Form *to_type_form = call->as.list.items[2];
    /* We don't validate the target type for v1; just require it exists */
    if (!to_type_form) {
        diag_emit(DIAG_ERROR, call->span, "unsafe-cast requires a target type");
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_unsafe_cast, value->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "unsafe-cast: builtin lookup failed");
        return NULL;
    }
    /* Result type is int for v1 */
    Type result_type = TYPE_INT;
    Expr *out = expr_new(e->arena, EX_BUILTIN, result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = value;
    args[1] = value; /* Placeholder for target type */
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_reinterpret(Elab *e, const Form *call) {
    /* (reinterpret value as-type) - bitwise reinterpretation
     * Similar to unsafe-cast but with compile-time size check requirement
     * For v1, we just check we're in an unsafe block */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "reinterpret requires exactly 2 arguments: (reinterpret value as-type)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "reinterpret requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;
    /* Compile-time size check: source and target types must have the same size */
    Form *to_type_form = call->as.list.items[2];
    TypeKind target_kind = TY_UNKNOWN;
    if (to_type_form && to_type_form->tag == F_KEYWORD) {
        target_kind = typekind_from_symbol(to_type_form->as.sym->name);
    } else if (to_type_form && to_type_form->tag == F_SYM) {
        target_kind = typekind_from_symbol(to_type_form->as.sym->name);
    }
    int src_size = type_size_bytes(value->type.kind);
    int dst_size = (target_kind != TY_UNKNOWN) ? type_size_bytes(target_kind) : 0;
    if (src_size > 0 && dst_size > 0 && src_size != dst_size) {
        diag_emit(DIAG_ERROR, call->span,
                  "reinterpret: size mismatch — source type '%s' (%d bytes) vs target type '%s' (%d bytes)",
                  type_name(value->type), src_size,
                  to_type_form->as.sym->name, dst_size);
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_reinterpret, value->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "reinterpret: builtin lookup failed");
        return NULL;
    }
    Type result_type = TYPE_INT;
    Expr *out = expr_new(e->arena, EX_BUILTIN, result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = value;
    args[1] = value; /* Placeholder */
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_transmute(Elab *e, const Form *call) {
    /* (transmute value to-type) - type-punning with compile-time size check */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "transmute requires exactly 2 arguments: (transmute value to-type)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "transmute requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;

    /* Resolve the target type from the keyword or symbol form. */
    Form *to_type_form = call->as.list.items[2];
    TypeKind target_kind = TY_UNKNOWN;
    if (to_type_form->tag == F_KEYWORD || to_type_form->tag == F_SYM) {
        target_kind = typekind_from_symbol(to_type_form->as.sym->name);
    }

    /* Compile-time size check: source and target types must have the same size. */
    int src_size = type_size_bytes(value->type.kind);
    int dst_size = (target_kind != TY_UNKNOWN) ? type_size_bytes(target_kind) : 0;
    if (src_size > 0 && dst_size > 0 && src_size != dst_size) {
        diag_emit(DIAG_ERROR, call->span,
                  "transmute: size mismatch -- source type '%s' (%d bytes) vs "
                  "target type '%s' (%d bytes)",
                  type_name(value->type), src_size,
                  to_type_form->as.sym->name, dst_size);
        return NULL;
    }

    const BuiltinSpec *spec = builtin_lookup(e->sym_transmute, value->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "transmute: builtin lookup failed");
        return NULL;
    }
    Type result_type = (target_kind != TY_UNKNOWN) ? (Type){ .kind = target_kind,
        .copy_kind = CK_COPY, .n_lifetimes = 0 } : TYPE_INT;
    Expr *out = expr_new(e->arena, EX_BUILTIN, result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 1 * sizeof(Expr *));
    args[0] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_array_get_unchecked(Elab *e, const Form *call) {
    /* (array-get-unchecked ptr index) - get element at index without bounds check */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires exactly 2 arguments: (array-get-unchecked ptr index)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *index = elab_form(e, call->as.list.items[2]);
    if (!index) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires ptr<void> as first argument, got %s",
                  type_name(ptr->type));
        return NULL;
    }
    if (index->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires int as second argument, got %s",
                  type_name(index->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_array_get_unchecked, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "array-get-unchecked: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = index;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_array_set_unchecked(Elab *e, const Form *call) {
    /* (array-set-unchecked ptr index value) - set element at index without bounds check */
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires exactly 3 arguments: (array-set-unchecked ptr index value)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *index = elab_form(e, call->as.list.items[2]);
    if (!index) return NULL;
    Expr *value = elab_form(e, call->as.list.items[3]);
    if (!value) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires ptr<void> as first argument, got %s",
                  type_name(ptr->type));
        return NULL;
    }
    if (index->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires int as second argument, got %s",
                  type_name(index->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_array_set_unchecked, ptr->type, 3);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "array-set-unchecked: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 3 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = index;
    args[2] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 3;
    return out;
}

Expr *elab_raw_malloc(Elab *e, const Form *call) {
    /* (raw-malloc size) - allocate raw memory */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-malloc requires exactly 1 argument: (raw-malloc size)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-malloc requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *size = elab_form(e, call->as.list.items[1]);
    if (!size) return NULL;
    if (size->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-malloc requires int as argument, got %s", type_name(size->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_malloc, size->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-malloc: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = size;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_raw_free(Elab *e, const Form *call) {
    /* (raw-free ptr) - free raw memory */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-free requires exactly 1 argument: (raw-free ptr)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-free requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-free requires ptr<void> as argument, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_free, ptr->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-free: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = ptr;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_raw_realloc(Elab *e, const Form *call) {
    /* (raw-realloc ptr new-size) - reallocate raw memory */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires exactly 2 arguments: (raw-realloc ptr new-size)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *new_size = elab_form(e, call->as.list.items[2]);
    if (!new_size) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires ptr<void> as first argument, got %s",
                  type_name(ptr->type));
        return NULL;
    }
    if (new_size->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires int as second argument, got %s",
                  type_name(new_size->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_realloc, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-realloc: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = new_size;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_raw_memcpy(Elab *e, const Form *call) {
    /* (raw-memcpy dest src n) - copy memory */
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires exactly 3 arguments: (raw-memcpy dest src n)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *dest = elab_form(e, call->as.list.items[1]);
    if (!dest) return NULL;
    Expr *src = elab_form(e, call->as.list.items[2]);
    if (!src) return NULL;
    Expr *n = elab_form(e, call->as.list.items[3]);
    if (!n) return NULL;
    if (dest->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires ptr<void> as first argument, got %s",
                  type_name(dest->type));
        return NULL;
    }
    if (src->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires ptr<void> as second argument, got %s",
                  type_name(src->type));
        return NULL;
    }
    if (n->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires int as third argument, got %s",
                  type_name(n->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_memcpy, dest->type, 3);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-memcpy: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 3 * sizeof(Expr *));
    args[0] = dest;
    args[1] = src;
    args[2] = n;
    out->as.builtin.args = args;
    out->as.builtin.n = 3;
    return out;
}

Expr *elab_raw_memset(Elab *e, const Form *call) {
    /* (raw-memset dest byte n) - set memory to byte value */
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires exactly 3 arguments: (raw-memset dest byte n)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *dest = elab_form(e, call->as.list.items[1]);
    if (!dest) return NULL;
    Expr *byte_val = elab_form(e, call->as.list.items[2]);
    if (!byte_val) return NULL;
    Expr *n = elab_form(e, call->as.list.items[3]);
    if (!n) return NULL;
    if (dest->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires ptr<void> as first argument, got %s",
                  type_name(dest->type));
        return NULL;
    }
    if (byte_val->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires int as second argument, got %s",
                  type_name(byte_val->type));
        return NULL;
    }
    if (n->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires int as third argument, got %s",
                  type_name(n->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_memset, dest->type, 3);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-memset: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 3 * sizeof(Expr *));
    args[0] = dest;
    args[1] = byte_val;
    args[2] = n;
    out->as.builtin.args = args;
    out->as.builtin.n = 3;
    return out;
}

Expr *elab_c_call(Elab *e, const Form *call) {
    /* (c-call fn-name arg1 arg2 ...) - call a C function by name
     * For v1, we emit a direct C call. The function must be declared via extern-c.
     * We look up the function name in the scope and emit a call to it.
     * This is a simplified implementation for v1. */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "c-call requires at least a function name: (c-call fn-name arg1 ...)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "c-call requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "c-call: function name must be a symbol");
        return NULL;
    }
    /* Look up the function in the scope - it should be an extern-c declaration */
    Binding *b = scope_lookup(e->scope, name_f->as.sym);
    if (!b) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "c-call: function '%s' is not declared (use extern-c to declare it)",
                  name_f->as.sym->name);
        return NULL;
    }
    if (b->type.kind != TY_FN) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "c-call: '%s' is not a function", name_f->as.sym->name);
        return NULL;
    }
    /* Elaborate arguments */
    uint32_t n_args = call->as.list.len - 2;
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[i + 2]);
        if (!args[i]) return NULL;
    }
    /* Create a call expression */
    Expr *fn_expr = expr_new(e->arena, EX_VAR, b->type, name_f->span);
    fn_expr->as.var.binding = b;
    
    /* For v1, we use the simple call_ structure which stores a binding */
    Expr *out = expr_new(e->arena, EX_CALL, type_from_kind(TY_INT), call->span);
    out->as.call_.fn_binding = b;
    out->as.call_.args = args;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    return out;
}

Expr *elab_dlopen(Elab *e, const Form *call) {
    /* (dlopen path) - dynamic library open
     * For v1, we emit a call to dlopen with RTLD_LAZY flag
     * Returns ptr<void> (the library handle) */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlopen requires exactly 1 argument: (dlopen path)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlopen requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *path = elab_form(e, call->as.list.items[1]);
    if (!path) return NULL;
    if (path->type.kind != TY_CSTR) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlopen requires cstr as argument, got %s", type_name(path->type));
        return NULL;
    }
    /* Emit: dlopen(path, RTLD_LAZY) */
    const BuiltinSpec *spec = builtin_lookup(e->sym_dlopen, path->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "dlopen: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = path;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_dlsym(Elab *e, const Form *call) {
    /* (dlsym handle symbol) - get symbol address from dynamic library
     * For v1, we emit a call to dlsym
     * Returns ptr<void> (the symbol address) */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires exactly 2 arguments: (dlsym handle symbol)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *handle = elab_form(e, call->as.list.items[1]);
    if (!handle) return NULL;
    Expr *symbol = elab_form(e, call->as.list.items[2]);
    if (!symbol) return NULL;
    if (handle->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires ptr<void> as first argument, got %s",
                  type_name(handle->type));
        return NULL;
    }
    if (symbol->type.kind != TY_CSTR) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires cstr as second argument, got %s",
                  type_name(symbol->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_dlsym, handle->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "dlsym: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = handle;
    args[1] = symbol;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_dlclose(Elab *e, const Form *call) {
    /* (dlclose handle) - close dynamic library
     * For v1, we emit a call to dlclose
     * Returns int (0 on success, non-zero on error) */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlclose requires exactly 1 argument: (dlclose handle)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlclose requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *handle = elab_form(e, call->as.list.items[1]);
    if (!handle) return NULL;
    if (handle->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlclose requires ptr<void> as argument, got %s",
                  type_name(handle->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_dlclose, handle->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "dlclose: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = handle;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_unsafe(Elab *e, const Form *call) {
    /* (unsafe body...) — desugars to (handle (do body...) (Unsafe [] k) (resume k nil))
     * This wraps the body in a handler that discharges the Unsafe effect,
     * ensuring it cannot leak to the caller. */
    uint32_t n = call->as.list.len - 1;
    
    /* Phase U5: Linting for unsafe blocks */
    
    /* Check for nested unsafe blocks */
    if (g_unsafe_warn_nested && e->unsafe_depth > 0) {
        diag_emit(DIAG_WARNING, call->span,
                  "nested unsafe block: unsafe blocks should not be nested");
    }
    
    /* Check for ;;; SAFETY: comment */
    if (g_unsafe_require_safety) {
        /* For v1, we don't have access to the raw source to check for ;;; SAFETY: comments
         * So we just emit a warning that SAFETY comments are required */
        diag_emit(DIAG_WARNING, call->span,
                  "unsafe block requires a ;;; SAFETY: comment documenting why unsafe code is needed");
    }
    
    /* Track statistics */
    if (g_unsafe_stats_enabled) {
        g_unsafe_block_count++;
        g_unsafe_total_lines += n;
    }
    
    /* Check size threshold */
    if (g_unsafe_max_lines > 0 && n > g_unsafe_max_lines) {
        diag_emit(DIAG_WARNING, call->span,
                  "unsafe block has %u expressions (threshold: %u); consider breaking into smaller blocks",
                  n, g_unsafe_max_lines);
    }
    
    if (n == 0) {
        /* Empty unsafe block - warning */
        diag_emit(DIAG_WARNING, call->span,
                  "empty unsafe block has no effect");
        return e_nil(e, call->span);
    }

    /* Elaborate body expressions with increased unsafe_depth
     * (this allows unsafe-only operations inside the block) */
    Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
    e->unsafe_depth++;
    for (uint32_t i = 0; i < n; i++) {
        items[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!items[i]) {
            e->unsafe_depth--;
            return NULL;
        }
    }
    e->unsafe_depth--;

    /* Wrap multiple expressions in a do block */
    Expr *body;
    if (n == 1) {
        body = items[0];
    } else {
        body = expr_new(e->arena, EX_DO, items[n - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n;
    }

    /* Create handle expression with Unsafe handler
     * The handler just resumes with nil (Unsafe effect carries no value) */
    HandleCase *unsafe_case = arena_alloc(e->arena, sizeof(HandleCase));
    unsafe_case->effect_name = e->sym_effect_unsafe;
    unsafe_case->n_params = 0;
    unsafe_case->param_names = NULL;
    unsafe_case->param_bindings = NULL;
    
    /* Create k binding (dummy continuation for Unsafe which returns nil) */
    Binding *k_binding = binding_new(e, e->sym_k, TYPE_NIL, false, false, call->span);
    k_binding->type.copy_kind = CK_MOVE;
    unsafe_case->k_name = e->sym_k;
    unsafe_case->k_binding = k_binding;
    
    /* Create handler body: (resume k nil) */
    /* First create nil literal */
    Expr *nil_lit = expr_new(e->arena, EX_NIL_LIT, TYPE_NIL, call->span);
    
    /* Create k variable reference */
    Expr *k_var = expr_new(e->arena, EX_VAR, k_binding->type, call->span);
    k_var->as.var.binding = k_binding;
    
    /* Create resume expression directly (resume k nil) */
    ResumeExpr *resume = arena_alloc(e->arena, sizeof(ResumeExpr));
    resume->k = k_var;
    resume->value = nil_lit;
    
    Expr *resume_expr = expr_new(e->arena, EX_RESUME, TYPE_NIL, call->span);
    resume_expr->as.resume_.resume = resume;
    
    unsafe_case->body = resume_expr;

    /* Create handle expression */
    HandleExpr *handle = arena_alloc(e->arena, sizeof(HandleExpr));
    handle->body = body;
    handle->cases = unsafe_case;
    handle->n_cases = 1;
    handle->shallow = false;   /* arena mem is not zeroed; Unsafe is a deep marker */
    /* Mark this as the pure-marker Unsafe discharge so emit can run the body
     * in place rather than through the int64-truncating fiber result slot. */
    handle->is_unsafe_marker = true;

    Expr *out = expr_new(e->arena, EX_HANDLE, body->type, call->span);
    out->as.handle_.handle = handle;
    return out;
}
