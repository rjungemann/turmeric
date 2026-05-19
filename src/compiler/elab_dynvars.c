/* elab_dynvars.c -- elaboration for Clojure-style dynamic vars (-Xdynamic-vars).
 * Phase DV0: parse (defdynamic ...) and register DynVarEntry metadata.
 * No binding stack or codegen yet (those are DV1/DV2). */
#include "elab_internal.h"

/* ---- helpers ---- */

/* Return true if `name` matches the *earmuffs* convention: starts and ends
 * with '*'.  Used to emit TUR-W0600 when the convention is absent. */
static bool has_earmuffs(const char *name) {
    size_t len = strlen(name);
    return len >= 3 && name[0] == '*' && name[len - 1] == '*';
}

/* ---- public API ---- */

/* DV0: Look up a registered dynamic var by name.
 * Returns NULL if no matching entry exists. */
DynVarEntry *dynvar_lookup(const Elab *e, const Symbol *name) {
    for (uint32_t i = 0; i < e->n_dynvars; i++) {
        if (e->dynvar_entries[i]->name == name) return e->dynvar_entries[i];
    }
    return NULL;
}

/* DV0: Elaborate (defdynamic [^private] *name* :type root-expr).
 *
 * Syntax:
 *   (defdynamic *name* :type root-expr)
 *   (defdynamic ^private *name* :type root-expr)
 *
 * Checks performed:
 *   - Feature flag must be active              (hard error, no code)
 *   - Must be at module top-level              (TUR-E0604)
 *   - Value type must not be substructural     (TUR-E0603)
 *   - Name missing *earmuffs* convention       (TUR-W0600, non-fatal)
 *   - Root-expr type must match declared type  (TUR-E0602)
 *
 * On success registers a DynVarEntry and returns EX_DEFDYNAMIC (nil at runtime).
 */
Expr *elab_defdynamic(Elab *e, const Form *call) {
    if (!g_dynvar_enabled) {
        diag_emit(DIAG_ERROR, call->span,
                  "defdynamic requires -Xdynamic-vars");
        return NULL;
    }

    /* Minimum shape: (defdynamic *name* :type root-expr) — 4 items */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "defdynamic requires (defdynamic *name* :type root-expr)");
        return NULL;
    }

    /* Parse optional ^private visibility annotation */
    bool is_private = false;
    uint32_t name_idx = 1;
    if (call->as.list.len >= 5) {
        Form *maybe_priv = call->as.list.items[1];
        if (maybe_priv->tag == F_SYM
            && maybe_priv->as.sym == e->sym_caret_private) {
            is_private = true;
            name_idx = 2;
        }
    }

    /* Parse var name */
    Form *name_f = call->as.list.items[name_idx];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "defdynamic: var name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_f->as.sym;

    /* TUR-E0604: must be at module top-level */
    if (e->fn_body_depth > 0) {
        diag_emit_with_code(DIAG_ERROR, name_f->span, TUR_E0604_DYNVAR_NOT_TOPLEVEL,
                            "defdynamic: '%s' must be declared at module top-level, "
                            "not inside a function", name->name);
        return NULL;
    }

    /* Parse the type annotation (keyword or type-ann form) */
    Form *type_f = call->as.list.items[name_idx + 1];
    Type *vtype = NULL;
    if (type_f->tag == F_KEYWORD || type_f->tag == F_SYM) {
        /* :int  or  int  — simple keyword / symbol type */
        vtype = type_expr_from_form(e, type_f, NULL, NULL, NULL, 0);
    } else if (type_f->tag == F_TYPE_ANN && type_f->as.list.len > 0) {
        vtype = type_expr_from_form(e, type_f->as.list.items[0], NULL, NULL, NULL, 0);
    } else {
        diag_emit(DIAG_ERROR, type_f->span,
                  "defdynamic: expected type annotation, e.g. :int");
        return NULL;
    }
    if (!vtype) {
        diag_emit(DIAG_ERROR, type_f->span,
                  "defdynamic: unknown or invalid type");
        return NULL;
    }

    /* TUR-E0603: reject substructural (linear / move-only) value types */
    if (vtype->copy_kind == CK_LINEAR || vtype->copy_kind == CK_UNIQUE) {
        diag_emit_with_code(DIAG_ERROR, type_f->span, TUR_E0603_DYNVAR_SUBSTRUCTURAL_TYPE,
                            "defdynamic: '%s' -- dynamic vars may not hold substructural "
                            "(linear or move-only) types", name->name);
        return NULL;
    }

    /* TUR-W0600: *earmuffs* convention warning */
    if (!has_earmuffs(name->name)) {
        diag_emit_with_code(DIAG_WARNING, name_f->span, TUR_W0600_DYNVAR_NO_EARMUFFS,
                            "defdynamic: '%s' -- dynamic var names should use the "
                            "*earmuffs* convention (e.g. *%s*)", name->name, name->name);
    }

    /* Elaborate the root (initial) expression */
    Form *root_f = call->as.list.items[name_idx + 2];
    Expr *root_expr = elab_form(e, root_f);
    if (!root_expr) return NULL;

    /* TUR-E0602: root-expr type must match declared value type */
    if (root_expr->type.kind != vtype->kind) {
        diag_emit_with_code(DIAG_ERROR, root_f->span, TUR_E0602_DYNVAR_TYPE_MISMATCH,
                            "defdynamic: '%s' declared as '%s' but root expression has type '%s'",
                            name->name, type_name(*vtype), type_name(root_expr->type));
        return NULL;
    }

    /* Allocate and register a DynVarEntry */
    DynVarEntry *entry = (DynVarEntry *)arena_alloc(e->arena, sizeof(DynVarEntry));
    entry->name       = name;
    entry->value_type = *vtype;
    entry->index      = (int)e->n_dynvars;
    entry->is_private = is_private;

    if (e->n_dynvars >= e->cap_dynvars) {
        e->cap_dynvars = e->cap_dynvars ? e->cap_dynvars * 2 : 8;
        e->dynvar_entries = (DynVarEntry **)realloc(
            e->dynvar_entries, e->cap_dynvars * sizeof(DynVarEntry *));
    }
    e->dynvar_entries[e->n_dynvars++] = entry;

    /* Add a global binding for the var so later elaboration can resolve it */
    Type binding_type = type_dynvar(e->arena, *vtype);
    Binding *b = binding_new(e, name, binding_type, /*is_mut=*/false,
                             /*is_global=*/true, name_f->span);
    b->is_dynvar    = true;
    b->dynvar_entry = entry;
    scope_add(&e->global, b);

    /* Build and return the compile-time-only AST node */
    Expr *out = expr_new(e->arena, EX_DEFDYNAMIC, TYPE_NIL, call->span);
    out->as.defdynamic_.entry     = entry;
    out->as.defdynamic_.root_expr = root_expr;
    return out;
}
