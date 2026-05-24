/* elab_structs.c -- struct/ADT/GADT definitions, pattern matching, and borrow traits. */
#include "elab_internal.h"

/* ---- file-local helper forward declarations ---- */
static void parse_struct_field_type(const char *tname, uint32_t tlen,
    TypeKind *out_kind, TypeKind *out_inner);
static bool typekind_is_copy_for_struct(TypeKind k);
static Binding *scope_lookup_type_def(Scope *s, const Symbol *name);
static bool elab_is_forward_type(Elab *e, const Symbol *sym);
static Type gadt_resolve_type_from_form(Elab *e, const AdtDef *gadt, const Form *f,
    const SkolemEnv *senv);
static void gadt_build_skolem_env(SkolemEnv *out, const AdtDef *def,
    const CtorDef *ctor);
static TypeKind gadt_field_typekind_from_form(const Form *f);

/* Phase 11: defstruct - define a struct type
 * Syntax: (defstruct Name [:copy] [field1 :type1 field2 :type2 ...])
 * 
 * The :copy annotation indicates the struct is bitwise-copyable (all fields must be Copy).
 * Without :copy, the struct is move-only (default).
 * 
 * Returns an EX_DEF expression that defines the struct type at file scope.
 */

/* Helper: parse a field type keyword like "int", "rc<int>", "ref<bool>", "ptr<void>" */
static void parse_struct_field_type(const char *tname, uint32_t tlen,
                                     TypeKind *out_kind, TypeKind *out_inner) {
    *out_kind = TY_UNKNOWN;
    *out_inner = TY_UNKNOWN;

    if (tlen == 3  && memcmp(tname, "int",   3) == 0) { *out_kind = TY_INT;      return; }
    if (tlen == 5  && memcmp(tname, "int64", 5) == 0) { *out_kind = TY_INT;      return; }
    if (tlen == 4  && memcmp(tname, "bool",  4) == 0) { *out_kind = TY_BOOL;     return; }
    if (tlen == 5  && memcmp(tname, "float", 5) == 0) { *out_kind = TY_FLOAT;    return; }
    if (tlen == 7  && memcmp(tname, "float64", 7) == 0) { *out_kind = TY_FLOAT;  return; }
    if (tlen == 4  && memcmp(tname, "int8",  4) == 0) { *out_kind = TY_INT8;     return; }
    if (tlen == 5  && memcmp(tname, "int16", 5) == 0) { *out_kind = TY_INT16;    return; }
    if (tlen == 5  && memcmp(tname, "int32", 5) == 0) { *out_kind = TY_INT32;    return; }
    if (tlen == 5  && memcmp(tname, "uint8", 5) == 0) { *out_kind = TY_UINT8;    return; }
    if (tlen == 6  && memcmp(tname, "uint16", 6) == 0) { *out_kind = TY_UINT16;  return; }
    if (tlen == 6  && memcmp(tname, "uint32", 6) == 0) { *out_kind = TY_UINT32;  return; }
    if (tlen == 6  && memcmp(tname, "uint64", 6) == 0) { *out_kind = TY_UINT64;  return; }
    if (tlen == 7  && memcmp(tname, "float32", 7) == 0) { *out_kind = TY_FLOAT32; return; }
    if (tlen == 4  && memcmp(tname, "cstr",  4) == 0) { *out_kind = TY_CSTR;     return; }
    if (tlen == 3  && memcmp(tname, "nil",   3) == 0) { *out_kind = TY_NIL;      return; }
    if (tlen == 4  && memcmp(tname, "void",  4) == 0) { *out_kind = TY_NIL;      return; }
    if (tlen == 9  && memcmp(tname, "ptr<void>", 9) == 0) { *out_kind = TY_PTR_VOID; return; }
    /* Phase 16 v2: :fn field type — function pointer (may carry #{...} effect-row annotation) */
    if (tlen == 2  && memcmp(tname, "fn",    2) == 0) { *out_kind = TY_FN;       return; }

    /* Compound types: rc<T>, ref<T>, lref<T>, weak<T> */
    /* Parse the prefix and inner type */
    const char *prefix_rc   = "rc<";
    const char *prefix_ref  = "ref<";
    const char *prefix_lref = "lref<";
    const char *prefix_weak = "weak<";

    TypeKind prefix_kind = TY_UNKNOWN;
    uint32_t prefix_len = 0;
    if (tlen > 3 && memcmp(tname, prefix_rc, 3) == 0)   { prefix_kind = TY_RC;   prefix_len = 3; }
    if (tlen > 4 && memcmp(tname, prefix_ref, 4) == 0)  { prefix_kind = TY_REF;  prefix_len = 4; }
    if (tlen > 5 && memcmp(tname, prefix_lref, 5) == 0) { prefix_kind = TY_LREF; prefix_len = 5; }
    if (tlen > 5 && memcmp(tname, prefix_weak, 5) == 0) { prefix_kind = TY_WEAK; prefix_len = 5; }

    if (prefix_kind != TY_UNKNOWN && prefix_len > 0 && tname[tlen - 1] == '>') {
        const char *inner_name = tname + prefix_len;
        uint32_t inner_len = tlen - prefix_len - 1; /* strip trailing '>' */
        TypeKind inner_kind = TY_UNKNOWN, dummy = TY_UNKNOWN;
        parse_struct_field_type(inner_name, inner_len, &inner_kind, &dummy);
        *out_kind = prefix_kind;
        *out_inner = inner_kind;
        return;
    }

    /* Unknown type - leave as TY_UNKNOWN */
}

/* Helper: check if a TypeKind is considered copy */
static bool typekind_is_copy_for_struct(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_BOOL: case TY_FLOAT: case TY_CSTR:
        case TY_PTR_VOID: case TY_NIL:
        case TY_INT8: case TY_INT16: case TY_INT32:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32:
        /* Phase 16 v2: :fn fields are stored as int64_t at the C level, so they are
         * trivially copyable (function pointer stored as an integer value). */
        case TY_FN:
            return true;
        default:
            return false;
    }
}

/* Phase RF0: Look up a binding for a user-defined struct or ADT type by name.
 * Unlike scope_lookup (which returns the most recent binding), this searches all
 * bindings for one with kind TY_STRUCT or TY_ADT.  Needed because a constructor
 * with the same name as its type (e.g. (defdata Expr (Expr :ExprNode))) shadows
 * the type binding with a TY_FN constructor binding. */
static Binding *scope_lookup_type_def(Scope *s, const Symbol *name) {
    for (Scope *cur = s; cur; cur = cur->parent) {
        for (uint32_t i = cur->n; i > 0; i--) {
            Binding *b = cur->bindings[i - 1];
            if (b->name == name &&
                (b->type.kind == TY_STRUCT || b->type.kind == TY_ADT)) {
                return b;
            }
        }
    }
    return NULL;
}

/* Phase DS3: when a defstruct field is annotated `rc<Name>`, look up `Name`
 * as a struct so the resulting Type can carry the StructDef alongside the
 * inner TypeKind.  Returns NULL when Name isn't an in-scope struct (the
 * field still works as an opaque RcControlBlock *, just without field-
 * resolution support through the rc wrapper). */
static StructDef *lookup_rc_inner_struct_def(Elab *e, const char *tname, uint32_t tlen) {
    if (tlen <= 4 || memcmp(tname, "rc<", 3) != 0 || tname[tlen - 1] != '>') {
        return NULL;
    }
    const char *inner_name = tname + 3;
    uint32_t inner_len = tlen - 4;  /* strip "rc<" and ">" */
    const Symbol *sym = symtab_intern(e->st, strslice(inner_name, inner_len));
    Binding *tb = scope_lookup_type_def(e->scope, sym);
    if (tb && tb->type.kind == TY_STRUCT) {
        return tb->type.as.struct_.def;
    }
    return NULL;
}

/* Phase RF0: Check if a symbol was registered as a forward-declared type stub */
static bool elab_is_forward_type(Elab *e, const Symbol *sym) {
    for (uint32_t i = 0; i < e->n_forward_type_syms; i++) {
        if (e->forward_type_syms[i] == sym) return true;
    }
    return false;
}

/* Phase RF0: Add a symbol to the forward-declared types list */
void elab_add_forward_type(Elab *e, const Symbol *sym) {
    if (e->n_forward_type_syms >= e->cap_forward_type_syms) {
        e->cap_forward_type_syms = e->cap_forward_type_syms ? e->cap_forward_type_syms * 2 : 8;
        e->forward_type_syms = (const Symbol **)realloc(e->forward_type_syms,
            e->cap_forward_type_syms * sizeof(Symbol *));
    }
    e->forward_type_syms[e->n_forward_type_syms++] = sym;
}

/* Helper: add StructDef to the elab registry */
void elab_register_struct_def(Elab *e, StructDef *def) {
    if (e->n_struct_defs >= e->cap_struct_defs) {
        e->cap_struct_defs = e->cap_struct_defs ? e->cap_struct_defs * 2 : 8;
        e->struct_defs = (StructDef **)realloc(e->struct_defs,
            e->cap_struct_defs * sizeof(StructDef *));
    }
    e->struct_defs[e->n_struct_defs++] = def;
}

Expr *elab_defstruct(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "defstruct requires a name and field list: (defstruct Name [:copy] [f1 : T1 ...])");
        return NULL;
    }
    
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defstruct name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;
    
    /* Check for optional :copy / :move / :linear annotation */
    bool is_copy = false;
    bool is_linear = false;
    uint32_t fields_start_idx = 2;

    if (call->as.list.len >= 3) {
        Form *kw_form = call->as.list.items[2];
        if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_copy) {
            is_copy = true;
            fields_start_idx = 3;
        } else if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_move) {
            is_copy = false;
            fields_start_idx = 3;
        } else if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_linear) {
            /* LT4: :linear structs are exactly-once (CK_LINEAR). */
            is_linear = true;
            fields_start_idx = 3;
        }
    }
    
    /* Phase TM0: optional type-parameter vector [K V ...] before field definitions.
     * If the next form is a vector containing only symbols (no keyword annotations),
     * treat it as a type-param list; field defs then come as separate list forms.
     * Old syntax [field :type ...] is unchanged (vector contains keywords). */
    const char **type_params_arr = NULL;
    uint8_t n_type_params_v = 0;
    bool new_field_syntax = false;

    if (fields_start_idx < call->as.list.len &&
        call->as.list.items[fields_start_idx]->tag == F_VEC) {
        Form *maybe_tp = call->as.list.items[fields_start_idx];
        bool all_syms = true;
        for (uint32_t pi = 0; pi < maybe_tp->as.list.len; pi++) {
            if (maybe_tp->as.list.items[pi]->tag != F_SYM) {
                all_syms = false;
                break;
            }
        }
        if (all_syms && maybe_tp->as.list.len > 0) {
            /* This is a type-params list; remaining forms are (field :type) lists */
            n_type_params_v = (uint8_t)maybe_tp->as.list.len;
            type_params_arr = (const char **)arena_alloc(e->arena,
                n_type_params_v * sizeof(char *));
            for (uint8_t pi = 0; pi < n_type_params_v; pi++) {
                type_params_arr[pi] = maybe_tp->as.list.items[pi]->as.sym->name;
            }
            fields_start_idx++;
            new_field_syntax = true;
        }
    }

    if (call->as.list.len < fields_start_idx + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "defstruct requires a field list");
        return NULL;
    }

    /* For new-style syntax, fields are F_LIST forms; for old-style, one F_VEC. */
    Form *fields_form = NULL;
    if (!new_field_syntax) {
        fields_form = call->as.list.items[fields_start_idx];
        if (fields_form->tag != F_VEC) {
            diag_emit(DIAG_ERROR, fields_form->span,
                      "defstruct field list must be a vector [f1 : T1 f2 : T2 ...]");
            return NULL;
        }
    }
    
    /* Phase RF0: allow re-elaboration of forward-declared stub types */
    bool is_forward_stub = false;
    Binding *existing_b = scope_lookup(e->scope, name);
    if (existing_b) {
        if (elab_is_forward_type(e, name)) {
            is_forward_stub = true;
        } else {
            diag_emit(DIAG_ERROR, name_form->span,
                      "defstruct: '%s' is already defined", name->name);
            return NULL;
        }
    }

    /* Phase TM0: count actual fields for both old-style and new-style syntax. */
    uint32_t actual_n_fields = 0;
    if (new_field_syntax) {
        /* New style: each remaining item in call->as.list is an F_LIST (field-name :type) */
        for (uint32_t fi = fields_start_idx; fi < call->as.list.len; fi++) {
            Form *ff = call->as.list.items[fi];
            if (ff->tag == F_LIST && ff->as.list.len >= 2) {
                actual_n_fields++;
            }
        }
        if (actual_n_fields == 0) {
            diag_emit(DIAG_ERROR, call->span,
                      "defstruct requires at least one field definition");
            return NULL;
        }
    } else {
        /* Old style: fields_form is the vector [name :type ...] */
        uint32_t n_items = fields_form->as.list.len;
        if (n_items == 0) {
            diag_emit(DIAG_ERROR, fields_form->span,
                      "defstruct field list cannot be empty");
            return NULL;
        }
        /* Phase 16 v2: Field list may contain optional #{...} after :fn type annotations.
         * Pre-scan to count actual fields and validate structure. */
        uint32_t scan = 0;
        while (scan < n_items) {
            if (fields_form->as.list.items[scan]->tag != F_SYM) {
                diag_emit(DIAG_ERROR, fields_form->as.list.items[scan]->span,
                          "defstruct field list: expected field name symbol");
                return NULL;
            }
            scan++; /* consume name */
            if (scan >= n_items) {
                diag_emit(DIAG_ERROR, fields_form->span,
                          "defstruct field list must have [name :type ...] pairs");
                return NULL;
            }
            const Form *type_tok = fields_form->as.list.items[scan];
            if (type_tok->tag == F_TYPE_ANN) type_tok = type_tok->as.list.items[0];
            /* F8 (cross-plan-followups): F_LIST permitted -- compound type
             * forms like (exists [a] [(Show a)] a), (Vec int), (forall ...).
             * Actual parsing happens in the main loop via
             * type_expr_from_form. */
            if (type_tok->tag != F_KEYWORD && type_tok->tag != F_SYM &&
                type_tok->tag != F_LIST) {
                diag_emit(DIAG_ERROR, fields_form->span,
                          "defstruct field list must have [name :type ...] pairs");
                return NULL;
            }
            const char *tname = NULL;
            uint32_t tlen = 0;
            if (type_tok->tag == F_KEYWORD || type_tok->tag == F_SYM) {
                tname = type_tok->as.sym->name;
                tlen  = type_tok->as.sym->len;
            }
            scan++; /* consume type keyword (or compound type form) */
            /* Optional #{...} effect-row only for :fn fields */
            bool is_fn_field = (tname && tlen == 2 && memcmp(tname, "fn", 2) == 0);
            if (is_fn_field && scan < n_items &&
                fields_form->as.list.items[scan]->tag == F_MAP) {
                scan++; /* consume #{...} annotation */
            }
            actual_n_fields++;
        }
    }

    /* Phase RF0: Allocate (or reuse the forward stub) StructDef and register
     * in global scope BEFORE parsing fields, so that self-referential and
     * mutually-recursive field type annotations resolve correctly. */
    StructDef *def;
    Binding *b;
    if (is_forward_stub) {
        /* Reuse the pre-registered stub and fill it in */
        b = existing_b;
        def = b->type.as.struct_.def;
        def->n_fields = actual_n_fields;
        def->fields = (StructField *)arena_alloc(e->arena, actual_n_fields * sizeof(StructField));
        memset(def->fields, 0, actual_n_fields * sizeof(StructField));  /* F8: zero full_type and other fields */
        def->is_copy = is_copy;
        def->is_linear = is_linear; /* LT4 */
        def->needs_drop_glue = false;
        def->origin_file_id = call->span.file_id;
        /* Phase TM0 */
        def->type_params = type_params_arr;
        def->n_type_params = n_type_params_v;
        /* Already in global scope and elab registry from the pre-pass */
    } else {
        def = (StructDef *)arena_alloc(e->arena, sizeof(StructDef));
        def->name = name->name;
        def->n_fields = actual_n_fields;
        def->fields = (StructField *)arena_alloc(e->arena, actual_n_fields * sizeof(StructField));
        memset(def->fields, 0, actual_n_fields * sizeof(StructField));  /* F8: zero full_type and other fields */
        def->is_copy = is_copy;
        def->is_linear = is_linear; /* LT4 */
        def->needs_drop_glue = false;
        /* Phase HKT-P4: record the file that defined this struct. */
        def->origin_file_id = call->span.file_id;
        /* Phase TM0 */
        def->type_params = type_params_arr;
        def->n_type_params = n_type_params_v;

        Type struct_type = type_struct(def);
        b = binding_new(e, name, struct_type, false, true, name_form->span);
        scope_add(&e->global, b);
        elab_register_struct_def(e, def);
    }

    /* Parse fields -- two paths: new-style (field-list forms) vs old-style (flat vector). */
    if (new_field_syntax) {
        /* Phase TM0 new-style: each (field-name :type) is a separate F_LIST item. */
        uint32_t fi = 0;
        for (uint32_t ci = fields_start_idx; ci < call->as.list.len && fi < actual_n_fields; ci++) {
            Form *ff = call->as.list.items[ci];
            if (ff->tag != F_LIST || ff->as.list.len < 2) continue;
            Form *field_name_form = ff->as.list.items[0];
            Form *field_type_form = ff->as.list.items[1];
            const Form *type_name_form = (field_type_form->tag == F_TYPE_ANN)
                ? field_type_form->as.list.items[0] : field_type_form;
            TypeKind fkind = TY_UNKNOWN, finner = TY_UNKNOWN;
            Type *full_type = NULL;

            /* F8 (cross-plan-followups): if the field type is a compound
             * form (F_LIST -- e.g. (exists [a] [(C a)] a), (Vec int)),
             * route through type_expr_from_form to get a full Type and
             * derive the C-level kind from that.  TY_APP, TY_EXISTS,
             * TY_FORALL all lower to int64_t at the C level (opaque
             * heap pointer), so storage layout is unchanged. */
            if (type_name_form->tag == F_LIST) {
                Type *t = type_expr_from_form(e, (Form *)type_name_form,
                    NULL, NULL, NULL, 0);
                if (!t) return NULL;
                full_type = t;
                if (t->kind == TY_APP || t->kind == TY_EXISTS ||
                    t->kind == TY_FORALL) {
                    fkind = TY_INT;
                    finner = TY_UNKNOWN;
                } else {
                    fkind = t->kind;
                    finner = TY_UNKNOWN;
                }
            } else {
                const char *tname = type_name_form->as.sym->name;
                uint32_t tlen = type_name_form->as.sym->len;
                parse_struct_field_type(tname, tlen, &fkind, &finner);
                if (fkind == TY_UNKNOWN) {
                    const Symbol *type_sym = symtab_intern(e->st, strslice(tname, tlen));
                    Binding *tb = scope_lookup_type_def(e->scope, type_sym);
                    if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
                        fkind = TY_INT;
                        finner = TY_UNKNOWN;
                    } else {
                        diag_emit(DIAG_ERROR, field_type_form->span,
                                  "defstruct field '%s' has unrecognized type :%s",
                                  field_name_form->as.sym->name, tname);
                        return NULL;
                    }
                } else if (fkind == TY_RC && finner == TY_UNKNOWN) {
                    /* DS3: rc<Name> over a user-defined struct -- carry the
                     * StructDef so receivers of rc<Name>-typed values can do
                     * field access / set! through the rc wrapper. */
                    StructDef *inner_def = lookup_rc_inner_struct_def(e, tname, tlen);
                    if (inner_def) {
                        finner = TY_STRUCT;
                        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *t = type_rc_struct(inner_def);
                        full_type = t;
                    }
                }
            }
            if (g_linear_enabled && is_copy && typekind_default_copy_kind(fkind) == CK_LINEAR) {
                diag_emit_with_code(DIAG_ERROR, field_type_form->span,
                                    TUR_E0102_LINEAR_COPY,
                                    "cannot copy linear field '%s' -- "
                                    "linear values cannot appear in :copy structs",
                                    field_name_form->as.sym->name);
                return NULL;
            }
            if (is_copy && !typekind_is_copy_for_struct(fkind)) {
                if (full_type) {
                    diag_emit(DIAG_ERROR, field_type_form->span,
                              "defstruct: field '%s' has non-copy compound type and cannot be used in :copy struct",
                              field_name_form->as.sym->name);
                } else {
                    diag_emit(DIAG_ERROR, field_type_form->span,
                              "defstruct: field '%s' has non-copy type :%s and cannot be used in :copy struct",
                              field_name_form->as.sym->name,
                              type_name_form->as.sym->name);
                }
                return NULL;
            }
            def->fields[fi].name = field_name_form->as.sym->name;
            def->fields[fi].kind = fkind;
            def->fields[fi].inner_kind = finner;
            def->fields[fi].effect_row = NULL;
            def->fields[fi].full_type = full_type;
            if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
                def->needs_drop_glue = true;
            }
            fi++;
        }
    } else {
        /* Old-style: flat [name :type name :type ...] vector */
        uint32_t n_items = fields_form->as.list.len;
        uint32_t scan = 0;
        for (uint32_t fi = 0; fi < actual_n_fields; fi++) {
            Form *field_name_form = fields_form->as.list.items[scan++];
            Form *field_type_form = fields_form->as.list.items[scan++];
            const Form *type_name_form = (field_type_form->tag == F_TYPE_ANN)
                ? field_type_form->as.list.items[0] : field_type_form;
            TypeKind fkind = TY_UNKNOWN, finner = TY_UNKNOWN;
            Type *full_type = NULL;

            /* F8 (cross-plan-followups): F_LIST compound field type
             * (e.g. (exists ...), (Vec int)) -- route through
             * type_expr_from_form and store the full Type on the
             * StructField for use sites. */
            if (type_name_form->tag == F_LIST) {
                Type *t = type_expr_from_form(e, (Form *)type_name_form,
                    NULL, NULL, NULL, 0);
                if (!t) return NULL;
                full_type = t;
                if (t->kind == TY_APP || t->kind == TY_EXISTS ||
                    t->kind == TY_FORALL) {
                    fkind = TY_INT;
                    finner = TY_UNKNOWN;
                } else {
                    fkind = t->kind;
                    finner = TY_UNKNOWN;
                }
            } else {
                const char *tname = type_name_form->as.sym->name;
                uint32_t tlen = type_name_form->as.sym->len;
                parse_struct_field_type(tname, tlen, &fkind, &finner);

                if (fkind == TY_UNKNOWN) {
                    /* Phase RF0: fall back to user-defined type lookup.  Any struct or
                     * ADT is heap-allocated and stored as an opaque int64_t pointer, so
                     * it is safe to use as a recursive field type without any layout change. */
                    const Symbol *type_sym = symtab_intern(e->st, strslice(tname, tlen));
                    Binding *tb = scope_lookup_type_def(e->scope, type_sym);
                    if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
                        fkind = TY_INT;
                        finner = TY_UNKNOWN;
                    } else {
                        diag_emit(DIAG_ERROR, field_type_form->span,
                                  "defstruct field '%s' has unrecognized type :%s",
                                  field_name_form->as.sym->name, tname);
                        return NULL;
                    }
                } else if (fkind == TY_RC && finner == TY_UNKNOWN) {
                    StructDef *inner_def = lookup_rc_inner_struct_def(e, tname, tlen);
                    if (inner_def) {
                        finner = TY_STRUCT;
                        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *t = type_rc_struct(inner_def);
                        full_type = t;
                    }
                }
            }

            /* LT1: E0102 -- linear fields cannot appear in :copy structs.
             * A field of type lref<T> (CK_LINEAR) makes the struct non-copyable,
             * which is incompatible with :copy semantics. :move structs may hold lref<T>. */
            if (g_linear_enabled && is_copy && typekind_default_copy_kind(fkind) == CK_LINEAR) {
                diag_emit_with_code(DIAG_ERROR, field_type_form->span,
                                    TUR_E0102_LINEAR_COPY,
                                    "cannot copy linear field '%s' -- "
                                    "linear values cannot appear in :copy structs",
                                    field_name_form->as.sym->name);
                return NULL;
            }
            /* :copy struct validation: all fields must be copy */
            if (is_copy && !typekind_is_copy_for_struct(fkind)) {
                /* F8: when the field was parsed via type_expr_from_form
                 * the original simple type name isn't captured; fall back
                 * to a generic message. */
                if (type_name_form->tag == F_LIST) {
                    diag_emit(DIAG_ERROR, field_type_form->span,
                              "defstruct: field '%s' has non-copy compound type and cannot be used in :copy struct",
                              field_name_form->as.sym->name);
                } else {
                    diag_emit(DIAG_ERROR, field_type_form->span,
                              "defstruct: field '%s' has non-copy type :%s and cannot be used in :copy struct",
                              field_name_form->as.sym->name,
                              type_name_form->as.sym->name);
                }
                return NULL;
            }

            def->fields[fi].name = field_name_form->as.sym->name;
            def->fields[fi].kind = fkind;
            def->fields[fi].inner_kind = finner;
            def->fields[fi].effect_row = NULL;
            def->fields[fi].full_type = full_type;

            /* Phase 16 v2: parse optional #{...} effect-row annotation for :fn fields */
            if (fkind == TY_FN && scan < n_items &&
                fields_form->as.list.items[scan]->tag == F_MAP) {
                Form *row_form = fields_form->as.list.items[scan++];
                uint8_t n_sym = (uint8_t)row_form->as.list.len;
                const Symbol **syms = (const Symbol **)arena_alloc(e->arena,
                                        (n_sym ? n_sym : 1) * sizeof(Symbol *));
                uint8_t n_valid = 0;
                for (uint32_t j = 0; j < row_form->as.list.len; j++) {
                    Form *item = row_form->as.list.items[j];
                    if (item->tag == F_SYM) {
                        syms[n_valid++] = item->as.sym;
                    }
                }
                def->fields[fi].effect_row = effect_row_unresolved(e->arena, syms, n_valid);
            }

            /* Check if this field requires drop glue */
            if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
                def->needs_drop_glue = true;
            }
        }
    }

    /* Return EX_DEF with struct_def populated.
     * Registration in global scope and elab registry was done above (RF0). */
    Expr *out = expr_new(e->arena, EX_DEF, TYPE_NIL, call->span);
    out->as.def_.binding = b;
    out->as.def_.init = NULL;
    out->as.def_.struct_def = def;
    return out;
}

/* SI4-C: defopaque -- named opaque int64_t newtype for REPL type tags.
 * Syntax: (defopaque Name :int)
 * Creates a StructDef with is_opaque=true; type_c_name → "int64_t" everywhere. */
Expr *elab_defopaque(Elab *e, const Form *call) {
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defopaque requires a name and base type: (defopaque Name :int)");
        return NULL;
    }
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span, "defopaque name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;
    Binding *existing_b = scope_lookup(e->scope, name);
    if (existing_b && !elab_is_forward_type(e, name)) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defopaque: '%s' is already defined", name->name);
        return NULL;
    }
    StructDef *def = (StructDef *)arena_alloc(e->arena, sizeof(StructDef));
    def->name = name->name;
    def->n_fields = 0;
    def->fields = NULL;
    def->is_copy = true;
    def->is_linear = false;
    def->needs_drop_glue = false;
    def->is_opaque = true;
    def->origin_file_id = call->span.file_id;
    /* Phase TM0: opaque types have no type params */
    def->type_params = NULL;
    def->n_type_params = 0;
    Type struct_type = type_struct(def);
    Binding *b = binding_new(e, name, struct_type, false, true, name_form->span);
    scope_add(&e->global, b);
    elab_register_struct_def(e, def);
    Expr *out = expr_new(e->arena, EX_DEF, TYPE_NIL, call->span);
    out->as.def_.binding = b;
    out->as.def_.init = NULL;
    out->as.def_.struct_def = def;
    return out;
}

/* Phase G0: Helper - register AdtDef in elab registry */
void elab_register_adt_def(Elab *e, AdtDef *def) {
    if (e->n_adt_defs >= e->cap_adt_defs) {
        e->cap_adt_defs = e->cap_adt_defs ? e->cap_adt_defs * 2 : 8;
        e->adt_defs = (AdtDef **)realloc(e->adt_defs,
            e->cap_adt_defs * sizeof(AdtDef *));
    }
    e->adt_defs[e->n_adt_defs++] = def;
}

/* Phase G0: defdata — define a sum type (ADT)
 * Syntax: (defdata Name [:copy]
 *           (Ctor1)
 *           (Ctor2 :T1 :T2)
 *           ...)
 */
Expr *elab_defdata(Elab *e, const Form *call) {
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defdata requires a name and at least one constructor: "
                  "(defdata Name (Ctor1) (Ctor2 :T1) ...)");
        return NULL;
    }

    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defdata name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Check for optional :copy annotation */
    bool is_copy = false;
    uint32_t ctors_start_idx = 2;
    if (call->as.list.len >= 3) {
        Form *kw_form = call->as.list.items[2];
        if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_copy) {
            is_copy = true;
            ctors_start_idx = 3;
        } else if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_move) {
            is_copy = false;
            ctors_start_idx = 3;
        }
    }

    /* Phase RF1: Check for an optional type-parameter vector [^f a b ...] between
     * the name (or :copy annotation) and the first constructor.  Type parameters
     * are stored on the AdtDef and used for documentation / future type-checking;
     * they do not affect C codegen (all values are int64_t pointers). */
    const char **type_params = NULL;
    uint8_t n_type_params = 0;
    if (ctors_start_idx < call->as.list.len &&
        call->as.list.items[ctors_start_idx]->tag == F_VEC) {
        Form *tp_form = call->as.list.items[ctors_start_idx];
        n_type_params = (uint8_t)tp_form->as.list.len;
        if (n_type_params > 0) {
            type_params = (const char **)arena_alloc(e->arena,
                              n_type_params * sizeof(char *));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                Form *pf = tp_form->as.list.items[pi];
                if (pf->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, pf->span,
                              "defdata: type parameter must be a symbol, e.g. a, ^f");
                    return NULL;
                }
                type_params[pi] = pf->as.sym->name;
            }
        }
        ctors_start_idx++;
    }

    /* Phase RF0: allow re-elaboration of forward-declared stub types */
    bool is_forward_stub_adt = false;
    Binding *existing_adt_b = scope_lookup(e->scope, name);
    if (existing_adt_b) {
        if (elab_is_forward_type(e, name)) {
            is_forward_stub_adt = true;
        } else {
            diag_emit(DIAG_ERROR, name_form->span,
                      "defdata: '%s' is already defined", name->name);
            return NULL;
        }
    }

    uint32_t n_ctors = call->as.list.len - ctors_start_idx;
    if (n_ctors == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "defdata: '%s' must have at least one constructor", name->name);
        return NULL;
    }

    /* Phase RF0: Allocate (or reuse forward stub) AdtDef and register BEFORE
     * parsing constructors so that self-referential and mutually-recursive
     * constructor field types resolve correctly. */
    AdtDef *def;
    Binding *adt_binding;
    Type adt_type;
    if (is_forward_stub_adt) {
        /* Reuse the pre-registered stub and fill it in */
        adt_binding = existing_adt_b;
        def = adt_binding->type.as.adt_.def;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        def->is_copy = is_copy;
        def->needs_drop_glue = false;
        def->is_gadt = false;
        def->type_params = type_params;
        def->n_type_params = n_type_params;
        /* Refresh adt_type from the def so copy_kind reflects is_copy correctly.
         * The pre-pass stub was created with is_copy=false; now that we know the
         * real is_copy flag, regenerate the type and update the binding. */
        adt_type = type_adt(def);
        /* Phase G1/HKT: Apply KIND_ARROW fix so that the kind check can detect
         * when a parameterized defdata type is used in a kind-* slot. */
        if (n_type_params >= 2)     adt_type.hkt_kind = KIND_ARROW2;
        else if (n_type_params == 1) adt_type.hkt_kind = KIND_ARROW;
        adt_binding->type = adt_type;
        /* Already in global scope and elab registry from the pre-pass */
    } else {
        def = (AdtDef *)arena_alloc(e->arena, sizeof(AdtDef));
        def->name = name->name;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        def->is_copy = is_copy;
        def->needs_drop_glue = false;
        /* Phase RF1: store type parameters */
        def->is_gadt = false;
        def->type_params = type_params;
        def->n_type_params = n_type_params;

        /* Pre-register ADT type so constructors can reference it.
         * Phase G1/HKT: Set hkt_kind based on type-parameter count so that
         * elab_defgadt's belt-and-suspenders kind check can detect when a
         * parameterized type constructor is used in a kind-* argument slot. */
        adt_type = type_adt(def);
        if (n_type_params >= 2)     adt_type.hkt_kind = KIND_ARROW2;
        else if (n_type_params == 1) adt_type.hkt_kind = KIND_ARROW;
        adt_binding = binding_new(e, name, adt_type, false, true, name_form->span);
        scope_add(&e->global, adt_binding);
    }

    /* Parse each constructor */
    for (uint32_t ci = 0; ci < n_ctors; ci++) {
        Form *ctor_form = call->as.list.items[ctors_start_idx + ci];
        if (ctor_form->tag != F_LIST) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defdata: constructor must be a list form (Ctor :T1 :T2 ...)");
            return NULL;
        }
        if (ctor_form->as.list.len < 1) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defdata: constructor form cannot be empty");
            return NULL;
        }
        Form *ctor_name_form = ctor_form->as.list.items[0];
        if (ctor_name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, ctor_name_form->span,
                      "defdata: constructor name must be a symbol");
            return NULL;
        }
        const Symbol *ctor_name = ctor_name_form->as.sym;

        uint32_t n_fields = ctor_form->as.list.len - 1;
        CtorDef *ctor = (CtorDef *)arena_alloc(e->arena, sizeof(CtorDef));
        ctor->name = ctor_name->name;
        ctor->n_fields = n_fields;
        ctor->fields = n_fields > 0
            ? (CtorField *)arena_alloc(e->arena, n_fields * sizeof(CtorField))
            : NULL;
        ctor->adt = def;
        ctor->tag = ci;
        ctor->result_type_form = NULL; /* Phase G1: NULL for defdata */
        /* F6-1 (cross-plan-followups): stash the raw field-type forms for
         * defdata ctors too (was previously NULL).  Without this, pattern
         * extraction at match time can only recover the C-level TypeKind
         * (TY_INT for ADT-typed fields), which makes a nested `match
         * inner ...` fail with "scrutinee must be an ADT type, got int". */
        ctor->field_forms = n_fields > 0
            ? (const struct Form **)arena_alloc(e->arena, n_fields * sizeof(const Form *))
            : NULL;

        /* Parse field types */
        for (uint32_t fi = 0; fi < n_fields; fi++) {
            Form *ft_form = ctor_form->as.list.items[1 + fi];
            if (ft_form->tag != F_KEYWORD) {
                diag_emit(DIAG_ERROR, ft_form->span,
                          "defdata: constructor field type must be a keyword like :int, :bool, :cstr");
                return NULL;
            }
            const char *tname = ft_form->as.sym->name;
            uint32_t tlen = ft_form->as.sym->len;
            TypeKind fkind, finner;
            parse_struct_field_type(tname, tlen, &fkind, &finner);
            if (fkind == TY_UNKNOWN) {
                /* Phase RF0: fall back to user-defined type lookup.
                 * All struct/ADT values are heap-allocated int64_t pointers. */
                const Symbol *type_sym = symtab_intern(e->st, strslice(tname, tlen));
                Binding *tb = scope_lookup_type_def(e->scope, type_sym);
                if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
                    fkind = TY_INT;
                    finner = TY_UNKNOWN;
                } else {
                    diag_emit(DIAG_ERROR, ft_form->span,
                              "defdata: field has unrecognized type :%s", tname);
                    return NULL;
                }
            }
            ctor->fields[fi].kind = fkind;
            ctor->fields[fi].inner_kind = finner;
            /* F6-1: also stash the raw form so match extraction can
             * recover the declared ADT/struct type. */
            if (ctor->field_forms) ctor->field_forms[fi] = ft_form;
            if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
                def->needs_drop_glue = true;
            }
        }

        def->ctors[ci] = ctor;

        /* Register constructor as a global binding.
         * 0-arg constructor: TY_ADT binding (it IS a value).
         * N-arg constructor: TY_FN binding (call it like a function). */
        if (n_fields == 0) {
            /* 0-arg: register as TY_ADT so calls to (Red) work */
            Binding *cb = binding_new(e, ctor_name, adt_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        } else {
            /* N-arg: register as TY_FN */
            TypeKind arg_kinds[MAX_FN_ARITY];
            uint8_t arity = (uint8_t)(n_fields > MAX_FN_ARITY ? MAX_FN_ARITY : n_fields);
            for (uint8_t fi = 0; fi < arity; fi++) {
                arg_kinds[fi] = ctor->fields[fi].kind;
            }
            Type fn_type = type_fn(arg_kinds, arity, TY_ADT);
            Binding *cb = binding_new(e, ctor_name, fn_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        }
    }

    /* Register ADT in elab registry (skip if it was a forward stub -- already registered) */
    if (!is_forward_stub_adt) {
        elab_register_adt_def(e, def);
    }

    /* Return EX_DEFDATA node */
    Expr *out = expr_new(e->arena, EX_DEFDATA, TYPE_NIL, call->span);
    out->as.defdata_.def = def;
    out->as.defdata_.binding = adt_binding;
    return out;
}

/* Phase G2: Look up a type parameter name in the current skolem environment.
 * Returns TY_UNKNOWN if not found. */
TypeKind gadt_skolem_lookup(const SkolemEnv *env, const char *name) {
    if (!env) return TY_UNKNOWN;
    for (uint8_t i = 0; i < env->n; i++) {
        if (strcmp(env->bindings[i].name, name) == 0)
            return env->bindings[i].kind;
    }
    return TY_UNKNOWN;
}

/* Phase G2: Resolve a type form to a full Type using the current skolem env.
 * For primitive symbols → concrete Type.
 * For type variable names → look up in senv; TY_TYVAR if unresolved.
 * For ADT reference forms `(AdtName ...)` → look up ADT in global scope.
 * Falls back to TY_INT (opaque int64_t) for unknown forms.
 */
static Type gadt_resolve_type_from_form(Elab *e, const AdtDef *gadt, const Form *f,
                                         const SkolemEnv *senv) {
    if (!f) return type_from_kind(TY_INT);

    if (f->tag == F_SYM) {
        const char *n = f->as.sym->name;
        /* Primitive types */
        if (strcmp(n, "int")    == 0) return type_from_kind(TY_INT);
        if (strcmp(n, "bool")   == 0) return type_from_kind(TY_BOOL);
        if (strcmp(n, "float")  == 0) return type_from_kind(TY_FLOAT);
        if (strcmp(n, "cstr")   == 0) return type_from_kind(TY_CSTR);
        if (strcmp(n, "ptr")    == 0) return type_from_kind(TY_PTR_VOID);
        if (strcmp(n, "int8")   == 0) return type_from_kind(TY_INT8);
        if (strcmp(n, "int16")  == 0) return type_from_kind(TY_INT16);
        if (strcmp(n, "int32")  == 0) return type_from_kind(TY_INT32);
        if (strcmp(n, "int64")  == 0) return type_from_kind(TY_INT64);
        if (strcmp(n, "uint8")  == 0) return type_from_kind(TY_UINT8);
        if (strcmp(n, "uint16") == 0) return type_from_kind(TY_UINT16);
        if (strcmp(n, "uint32") == 0) return type_from_kind(TY_UINT32);
        if (strcmp(n, "uint64") == 0) return type_from_kind(TY_UINT64);
        if (strcmp(n, "float32")== 0) return type_from_kind(TY_FLOAT32);
        if (strcmp(n, "float64")== 0) return type_from_kind(TY_FLOAT64);
        /* Type variable: look up in skolem env */
        TypeKind resolved = gadt_skolem_lookup(senv, n);
        if (resolved != TY_UNKNOWN) return type_from_kind(resolved);
        /* Unresolved type variable → anonymous TY_TYVAR (name=NULL signals skolem escape) */
        (void)gadt; /* suppress unused warning */
        return type_tyvar_named(NULL);
    }

    if (f->tag == F_LIST && f->as.list.len >= 1) {
        /* Possibly an ADT reference: (AdtName type-args...) */
        Form *head = f->as.list.items[0];
        if (head->tag == F_SYM) {
            Binding *b = scope_lookup(e->scope, head->as.sym);
            if (!b) b = scope_lookup(&e->global, head->as.sym);
            if (b && b->type.kind == TY_ADT && b->type.as.adt_.def) {
                return b->type; /* TY_ADT with the def pointer */
            }
            /* Phase HKT: kind-variable application (f a) where f : * -> *.
             * Return an anonymous TY_TYVAR so the arm body is accepted as
             * int64_t-sized (same runtime repr as TY_ADT/TY_APP). */
            if (b && b->type.kind == TY_TYVAR && b->type.hkt_kind == KIND_ARROW) {
                return type_tyvar_named(NULL);
            }
        }
    }

    return type_from_kind(TY_INT); /* fallback: opaque int64_t */
}

/* Phase G2: Build a SkolemEnv for a GADT constructor arm.
 * Parses the constructor's result_type_form (e.g. "(Expr int)") against
 * the ADT's type_params (e.g. ["a"]) to produce concrete bindings
 * such as {a → TY_INT}.  Only primitive-type args are mapped; type-variable
 * args are skipped (leaving those params unresolved → TY_TYVAR in fields). */
static void gadt_build_skolem_env(SkolemEnv *out, const AdtDef *def,
                                   const CtorDef *ctor) {
    out->n = 0;
    if (!ctor->result_type_form || def->n_type_params == 0) return;

    const Form *rt = ctor->result_type_form;
    /* rt should be (AdtName arg0 arg1 ...) */
    if (rt->tag != F_LIST || rt->as.list.len < 2) return;

    /* arg i is at items[1+i] */
    uint32_t n_args = rt->as.list.len - 1;
    uint32_t n_bind = (n_args < def->n_type_params) ? n_args : def->n_type_params;

    for (uint32_t i = 0; i < n_bind && out->n < MAX_SKOLEM_BINDINGS; i++) {
        Form *arg = rt->as.list.items[1 + i];
        const char *param_name = def->type_params[i];
        TypeKind k = TY_UNKNOWN;

        if (arg->tag == F_SYM) {
            const char *an = arg->as.sym->name;
            if (strcmp(an, "int")    == 0) k = TY_INT;
            else if (strcmp(an, "bool")   == 0) k = TY_BOOL;
            else if (strcmp(an, "float")  == 0) k = TY_FLOAT;
            else if (strcmp(an, "cstr")   == 0) k = TY_CSTR;
            else if (strcmp(an, "int8")   == 0) k = TY_INT8;
            else if (strcmp(an, "int16")  == 0) k = TY_INT16;
            else if (strcmp(an, "int32")  == 0) k = TY_INT32;
            else if (strcmp(an, "int64")  == 0) k = TY_INT64;
            else if (strcmp(an, "uint8")  == 0) k = TY_UINT8;
            else if (strcmp(an, "uint16") == 0) k = TY_UINT16;
            else if (strcmp(an, "uint32") == 0) k = TY_UINT32;
            else if (strcmp(an, "uint64") == 0) k = TY_UINT64;
            else if (strcmp(an, "float32")== 0) k = TY_FLOAT32;
            else if (strcmp(an, "float64")== 0) k = TY_FLOAT64;
            /* else: type variable or unknown — skip (param stays unresolved) */
        }
        /* List form like (Expr int) — ADT ref; treat as int64_t / TY_INT */
        else if (arg->tag == F_LIST) {
            k = TY_INT; /* opaque ADT reference */
        }

        if (k != TY_UNKNOWN) {
            out->bindings[out->n].name = param_name;
            out->bindings[out->n].kind = k;
            out->n++;
        }
    }
}

/* Phase G1: Map a simple type form to a TypeKind for codegen.
 * Handles primitive names (int, bool, cstr, etc.) and falls back to
 * TY_INT (opaque int64_t) for ADT references and type variables. */
static TypeKind gadt_field_typekind_from_form(const Form *f) {
    if (!f) return TY_INT;
    if (f->tag == F_SYM) {
        const char *n = f->as.sym->name;
        if (strcmp(n, "int")   == 0) return TY_INT;
        if (strcmp(n, "bool")  == 0) return TY_BOOL;
        if (strcmp(n, "float") == 0) return TY_FLOAT;
        if (strcmp(n, "cstr")  == 0) return TY_CSTR;
        if (strcmp(n, "ptr")   == 0) return TY_PTR_VOID;
        if (strcmp(n, "int8")  == 0) return TY_INT8;
        if (strcmp(n, "int16") == 0) return TY_INT16;
        if (strcmp(n, "int32") == 0) return TY_INT32;
        if (strcmp(n, "int64") == 0) return TY_INT64;
        if (strcmp(n, "uint8") == 0) return TY_UINT8;
        if (strcmp(n, "uint16")== 0) return TY_UINT16;
        if (strcmp(n, "uint32")== 0) return TY_UINT32;
        if (strcmp(n, "uint64")== 0) return TY_UINT64;
        if (strcmp(n, "float32")== 0) return TY_FLOAT32;
        if (strcmp(n, "float64")== 0) return TY_FLOAT64;
        /* Type variable or unknown type — opaque int64_t */
        return TY_INT;
    }
    /* List form like (Expr int) — ADT reference, opaque int64_t */
    return TY_INT;
}

/* Phase G1: defgadt — define a GADT (Generalized Algebraic Data Type).
 * Syntax: (defgadt Name [type-params...]
 *           (Ctor1 : return-type)
 *           (Ctor2 FieldType1 FieldType2 : return-type)
 *           ...)
 *
 * The ':' separator is a bare F_SYM(":") token.
 * Field types are forms appearing between the constructor name and ':'.
 * The return-type annotation is stored on the CtorDef for future G2 use.
 * Codegen is identical to defdata (tagged union).
 */
Expr *elab_defgadt(Elab *e, const Form *call) {
    if (!g_gadt_enabled) {
        diag_emit(DIAG_ERROR, call->span,
                  "defgadt requires the -Xgadt flag to enable GADT support\n"
                  "  hint: recompile with -Xgadt");
        return NULL;
    }
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defgadt requires a name, type-params, and constructors: "
                  "(defgadt Name [params] (Ctor : return-type) ...)");
        return NULL;
    }

    /* Parse name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span, "defgadt name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Parse type parameters — must be a vector [a b c] */
    uint32_t ctors_start_idx = 2;
    uint8_t n_type_params = 0;
    const char **type_params = NULL;
    if (call->as.list.len >= 3 && call->as.list.items[2]->tag == F_VEC) {
        Form *params_form = call->as.list.items[2];
        n_type_params = (uint8_t)params_form->as.list.len;
        if (n_type_params > 0) {
            type_params = (const char **)arena_alloc(e->arena,
                                                      n_type_params * sizeof(const char *));
            for (uint8_t i = 0; i < n_type_params; i++) {
                Form *pf = params_form->as.list.items[i];
                if (pf->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, pf->span,
                              "defgadt: type parameter must be a symbol");
                    return NULL;
                }
                type_params[i] = pf->as.sym->name;
            }
        }
        ctors_start_idx = 3;
    }

    if (call->as.list.len <= ctors_start_idx) {
        diag_emit(DIAG_ERROR, call->span,
                  "defgadt: '%s' must have at least one constructor", name->name);
        return NULL;
    }

    /* Phase RF0: allow re-elaboration of forward-declared stub types */
    bool is_forward_stub_gadt = false;
    Binding *existing_gadt_b = scope_lookup(e->scope, name);
    if (existing_gadt_b) {
        if (elab_is_forward_type(e, name)) {
            is_forward_stub_gadt = true;
        } else {
            diag_emit(DIAG_ERROR, name_form->span,
                      "defgadt: '%s' is already defined", name->name);
            return NULL;
        }
    }

    uint32_t n_ctors = call->as.list.len - ctors_start_idx;

    /* Phase RF0: Allocate (or reuse forward stub) AdtDef and register BEFORE
     * parsing constructors so that self-referential and mutually-recursive
     * constructor field types resolve correctly. */
    AdtDef *def;
    Binding *adt_binding;
    Type adt_type;
    if (is_forward_stub_gadt) {
        /* Reuse the pre-registered stub and fill it in */
        adt_binding = existing_gadt_b;
        def = adt_binding->type.as.adt_.def;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        def->is_copy = false;
        def->needs_drop_glue = false;
        def->is_gadt = true;
        def->type_params = type_params;
        def->n_type_params = n_type_params;
        adt_type = type_adt(def);
        /* Phase G1/HKT: Apply the same KIND_ARROW fix as the non-stub branch so
         * that kind checks see the correct kind for parameterized GADTs. */
        if (n_type_params >= 2)     adt_type.hkt_kind = KIND_ARROW2;
        else if (n_type_params == 1) adt_type.hkt_kind = KIND_ARROW;
        adt_binding->type = adt_type;
        /* Already in global scope and elab registry from the pre-pass */
    } else {
        def = (AdtDef *)arena_alloc(e->arena, sizeof(AdtDef));
        def->name = name->name;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        def->is_copy = false;
        def->needs_drop_glue = false;
        def->is_gadt = true;
        def->type_params = type_params;
        def->n_type_params = n_type_params;

        /* Pre-register ADT type so constructors can reference it.
         * Phase G1/HKT: Set hkt_kind based on type-parameter count so that
         * the belt-and-suspenders kind check can detect when a parameterized
         * GADT is used in a kind-* argument slot of another GADT. */
        adt_type = type_adt(def);
        if (n_type_params >= 2)     adt_type.hkt_kind = KIND_ARROW2;
        else if (n_type_params == 1) adt_type.hkt_kind = KIND_ARROW;
        adt_binding = binding_new(e, name, adt_type, false, true, name_form->span);
        scope_add(&e->global, adt_binding);
    }

    /* Parse each constructor */
    for (uint32_t ci = 0; ci < n_ctors; ci++) {
        Form *ctor_form = call->as.list.items[ctors_start_idx + ci];
        if (ctor_form->tag != F_LIST || ctor_form->as.list.len < 1) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defgadt: constructor must be a list form");
            return NULL;
        }
        Form *ctor_name_form = ctor_form->as.list.items[0];
        if (ctor_name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, ctor_name_form->span,
                      "defgadt: constructor name must be a symbol");
            return NULL;
        }
        const Symbol *ctor_name = ctor_name_form->as.sym;

        /* Find the ':' separator in the constructor form.
         * Format: (CtorName FieldType1 ... FieldTypeN : return-type-form)
         * The ':' may be a bare F_SYM(":") token (legacy) or an F_TYPE_ANN node
         * produced by the new `: type-expr` reader (Phase G3 compat). */
        int colon_idx = -1;
        bool type_ann_colon = false; /* true when the ':' was absorbed into F_TYPE_ANN */
        for (uint32_t fi = 1; fi < ctor_form->as.list.len; fi++) {
            Form *item = ctor_form->as.list.items[fi];
            if (item->tag == F_SYM && item->as.sym == e->sym_colon) {
                colon_idx = (int)fi;
                break;
            }
            if (item->tag == F_TYPE_ANN) {
                /* `: return-type` was folded into a single F_TYPE_ANN node.
                 * Treat this index as the separator; the return type is inside. */
                colon_idx = (int)fi;
                type_ann_colon = true;
                break;
            }
        }
        if (colon_idx < 0) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defgadt: constructor '%s' requires an explicit return-type annotation\n"
                      "  hint: add ': (return-type)' after the constructor name",
                      ctor_name->name);
            return NULL;
        }
        /* For the F_TYPE_ANN case the return-type form is the inner form;
         * for the bare-':' case it is the item immediately after. */
        Form *return_type_form;
        if (type_ann_colon) {
            Form *ann = ctor_form->as.list.items[colon_idx];
            if (ann->as.list.len < 1) {
                diag_emit(DIAG_ERROR, ann->span,
                          "defgadt: constructor '%s': missing return type after ':'",
                          ctor_name->name);
                return NULL;
            }
            return_type_form = ann->as.list.items[0];
        } else {
            if ((uint32_t)colon_idx + 1 >= ctor_form->as.list.len) {
                diag_emit(DIAG_ERROR, ctor_form->span,
                          "defgadt: constructor '%s': missing return type after ':'",
                          ctor_name->name);
                return NULL;
            }
            return_type_form = ctor_form->as.list.items[colon_idx + 1];
        }
        /* Validate: return type must mention the GADT name */
        bool mentions_gadt = false;
        if (return_type_form->tag == F_LIST && return_type_form->as.list.len >= 1) {
            Form *head = return_type_form->as.list.items[0];
            if (head->tag == F_SYM && strcmp(head->as.sym->name, name->name) == 0) {
                mentions_gadt = true;
            }
        } else if (return_type_form->tag == F_SYM &&
                   strcmp(return_type_form->as.sym->name, name->name) == 0) {
            mentions_gadt = true;
        }
        if (!mentions_gadt) {
            diag_emit(DIAG_ERROR, return_type_form->span,
                      "defgadt: constructor '%s' return type must be an application of '%s'",
                      ctor_name->name, name->name);
            return NULL;
        }

        /* Change 3: Validate that the number of type args in the return type
         * matches the GADT's declared type-parameter count. */
        if (return_type_form->tag == F_LIST) {
            uint32_t n_rt_args = return_type_form->as.list.len - 1; /* subtract head */
            if (n_type_params > 0 && n_rt_args != n_type_params) {
                diag_emit(DIAG_ERROR, return_type_form->span,
                          "defgadt: constructor '%s' return type has %u type argument(s) "
                          "but '%s' has %u type parameter(s)",
                          ctor_name->name, n_rt_args, name->name, n_type_params);
                return NULL;
            }
        }

        /* Change 4: Validate each type arg in the return type is a known type. */
        if (return_type_form->tag == F_LIST) {
            for (uint32_t ai = 1; ai < return_type_form->as.list.len; ai++) {
                Form *arg = return_type_form->as.list.items[ai];
                if (arg->tag == F_LIST) continue; /* type application -- ok */
                if (arg->tag != F_SYM) continue;  /* other forms -- ok */
                const char *an = arg->as.sym->name;
                /* Check if it's a bound type param */
                bool is_param = false;
                for (uint8_t pi = 0; pi < n_type_params; pi++) {
                    if (strcmp(type_params[pi], an) == 0) { is_param = true; break; }
                }
                if (is_param) continue;
                /* Check if it's a concrete primitive */
                static const char *primitives[] = {
                    "int", "bool", "float", "cstr", "nil", "void", "ptr",
                    "int8", "int16", "int32", "int64",
                    "uint8", "uint16", "uint32", "uint64",
                    "float32", "float64", NULL
                };
                bool is_prim = false;
                for (int pi = 0; primitives[pi]; pi++) {
                    if (strcmp(primitives[pi], an) == 0) { is_prim = true; break; }
                }
                if (is_prim) continue;
                /* Check if it's a known type in scope */
                const Symbol *type_sym = symtab_intern(e->st,
                    strslice(an, (uint32_t)strlen(an)));
                Binding *tb = scope_lookup_type_def(e->scope, type_sym);
                if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
                    /* Phase G1/HKT: Belt-and-suspenders kind check.
                     * All GADT type parameters have kind * in Phase G1.  A
                     * type constructor of kind * -> * (hkt_kind == KIND_ARROW)
                     * in a plain type-argument slot is a kind mismatch. */
                    if (tb->type.hkt_kind == KIND_ARROW ||
                            tb->type.hkt_kind == KIND_ARROW2) {
                        diag_emit_with_code(DIAG_ERROR, arg->span,
                            TUR_E0012_KIND_MISMATCH,
                            "kind mismatch (TUR-E0012): type argument '%s' in constructor "
                            "'%s' return type has kind '* -> *' but kind '*' is expected",
                            an, ctor_name->name);
                        return NULL;
                    }
                    continue;
                }
                /* Unknown -- error */
                diag_emit(DIAG_ERROR, arg->span,
                          "defgadt: unknown type argument '%s' in return type of constructor '%s' "
                          "(must be a type parameter, primitive type, or defined type)",
                          an, ctor_name->name);
                return NULL;
            }
        }

        /* Field types: items[1 .. colon_idx-1] */
        uint32_t n_fields = (uint32_t)(colon_idx - 1);
        CtorDef *ctor = (CtorDef *)arena_alloc(e->arena, sizeof(CtorDef));
        ctor->name = ctor_name->name;
        ctor->n_fields = n_fields;
        ctor->fields = n_fields > 0
            ? (CtorField *)arena_alloc(e->arena, n_fields * sizeof(CtorField))
            : NULL;
        ctor->adt = def;
        ctor->tag = ci;
        ctor->result_type_form = return_type_form;
        /* Phase G2: store raw field-type annotation forms for per-arm resolution */
        ctor->field_forms = n_fields > 0
            ? (const struct Form **)arena_alloc(e->arena, n_fields * sizeof(const Form *))
            : NULL;

        for (uint32_t fi = 0; fi < n_fields; fi++) {
            Form *ft_form = ctor_form->as.list.items[1 + fi];
            TypeKind fkind = gadt_field_typekind_from_form(ft_form);
            ctor->fields[fi].kind = fkind;
            ctor->fields[fi].inner_kind = TY_UNKNOWN;
            if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
                def->needs_drop_glue = true;
            }
            /* Phase G2: also stash the raw form for per-arm type resolution */
            if (ctor->field_forms) ctor->field_forms[fi] = ft_form;
        }
        def->ctors[ci] = ctor;

        /* Register constructor binding */
        if (n_fields == 0) {
            Binding *cb = binding_new(e, ctor_name, adt_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        } else {
            TypeKind arg_kinds[MAX_FN_ARITY];
            uint8_t arity = (uint8_t)(n_fields > MAX_FN_ARITY ? MAX_FN_ARITY : n_fields);
            for (uint8_t fi = 0; fi < arity; fi++) {
                arg_kinds[fi] = ctor->fields[fi].kind;
            }
            Type fn_type = type_fn(arg_kinds, arity, TY_ADT);
            Binding *cb = binding_new(e, ctor_name, fn_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        }
    }

    /* Register ADT in elab registry (skip if it was a forward stub -- already registered) */
    if (!is_forward_stub_gadt) {
        elab_register_adt_def(e, def);
    }

    Expr *out = expr_new(e->arena, EX_DEFGADT, TYPE_NIL, call->span);
    out->as.defgadt_.def = def;
    out->as.defgadt_.binding = adt_binding;
    return out;
}

/* Phase G3: coerce — (coerce eq x) where eq : (Equal a b), x : a → x : b
 * Zero-cost cast: the runtime representation of a and b are identical (both int64_t).
 * The equality proof eq is evaluated for side-effects but its value is discarded.
 * Error if eq is not a value of the built-in Equal GADT. */
Expr *elab_coerce(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "coerce requires exactly 2 arguments: (coerce eq x)");
        return NULL;
    }
    Expr *eq = elab_form(e, call->as.list.items[1]);
    if (!eq) return NULL;
    /* Verify eq has type Equal */
    if (eq->type.kind != TY_ADT ||
        !eq->type.as.adt_.def ||
        strcmp(eq->type.as.adt_.def->name, "Equal") != 0) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "coerce requires an (Equal a b) proof as first argument");
        return NULL;
    }
    Expr *x = elab_form(e, call->as.list.items[2]);
    if (!x) return NULL;
    /* SS1: Reject coerce whose source is a session channel.
     * Reinterpreting a Session endpoint at a different protocol step would
     * strand the linear resource; the Equal witness is insufficient to make
     * this safe without full protocol equality checking (SS3+). */
    if (g_sessions_enabled && x->type.kind == TY_SESSION) {
        diag_emit(DIAG_ERROR, call->span,
                  "coerce cannot reinterpret a session channel endpoint; "
                  "use session operations (send/recv/close/...) to advance the protocol");
        return NULL;
    }
    /* Zero-cost cast: return x unchanged (same runtime representation) */
    return x;
}

/* Phase G0: Helper - look up CtorDef by name across all known ADTs */
CtorDef *elab_lookup_ctor(Elab *e, const Symbol *name) {
    for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
        AdtDef *adt = e->adt_defs[ai];
        for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
            if (strcmp(adt->ctors[ci]->name, name->name) == 0) {
                return adt->ctors[ci];
            }
        }
    }
    return NULL;
}

/* Phase G0: match expression
 * Syntax: (match scrutinee
 *   (Ctor1 x y) body1
 *   (Ctor2 z)   body2
 *   _           default-body)
 * Arms are interleaved: pattern body pattern body ...
 */
Expr *elab_match(Elab *e, const Form *call) {
    /* call->as.list.items[0] = "match"
     * call->as.list.items[1] = scrutinee
     * call->as.list.items[2..] = pattern body pattern body ... */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "match requires a scrutinee: (match scrutinee pattern body ...)");
        return NULL;
    }
    /* Phase G4: Pre-scan arms to count and find per-arm start indices.
     * Arms can be (pat body) or (pat when guard body). */
    uint32_t arm_start[256];    /* start index of each arm's pattern */
    bool arm_has_guard[256];    /* whether the arm has a when-guard */
    uint32_t n_arms = 0;
    {
        uint32_t idx = 2; /* skip 'match' and scrutinee */
        while (idx < call->as.list.len) {
            if (n_arms >= 256) {
                diag_emit(DIAG_ERROR, call->span, "match: too many arms (max 256)");
                return NULL;
            }
            arm_start[n_arms] = idx;
            arm_has_guard[n_arms] = false;
            idx++; /* skip pattern */
            /* Check for optional 'when guard' */
            if (idx + 1 < call->as.list.len &&
                call->as.list.items[idx]->tag == F_SYM &&
                call->as.list.items[idx]->as.sym == e->sym_when) {
                arm_has_guard[n_arms] = true;
                idx += 2; /* skip 'when' and guard expr */
            }
            idx++; /* skip body */
            n_arms++;
        }
        /* Verify last arm ends exactly at list end */
        if (idx != call->as.list.len) {
            diag_emit(DIAG_ERROR, call->span,
                      "match: malformed arm list (missing body for last arm?)");
            return NULL;
        }
    }
    if (n_arms == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "match requires at least one arm: (match scrutinee pattern body)");
        return NULL;
    }

    /* Elaborate scrutinee */
    Expr *scrutinee = elab_form(e, call->as.list.items[1]);
    if (!scrutinee) return NULL;

    /* IT1: Union type match — when scrutinee is TY_UNION, handle type-narrowing patterns.
     * Pattern syntax: (varname : TypeName) or bare _ / variable for wildcard.
     * Returns early via the union match path. */
    if (g_union_types_enabled && scrutinee->type.kind == TY_UNION) {
        Type *union_t = &scrutinee->type;
        uint8_t n_members = union_t->as.union_.n_members;

        /* Track which union members are covered */
        bool *member_covered = (bool *)calloc(n_members, sizeof(bool));
        bool has_wildcard = false;
        Type result_type = TYPE_UNKNOWN;

        MatchArm *arms = (MatchArm *)arena_alloc(e->arena, n_arms * sizeof(MatchArm));

        for (uint32_t ai = 0; ai < n_arms; ai++) {
            Form *pat_form = call->as.list.items[2 + ai * 2];
            Form *body_form = call->as.list.items[3 + ai * 2];
            MatchPattern *pat = &arms[ai].pattern;
            memset(pat, 0, sizeof(MatchPattern));

            /* Wildcard: bare _ or bare variable symbol */
            if (pat_form->tag == F_SYM) {
                const Symbol *sym_wildcard = intern_cstr(e->st, "_");
                if (pat_form->as.sym == sym_wildcard) {
                    pat->is_wildcard = true;
                } else {
                    pat->is_var = true;
                    pat->var_sym = pat_form->as.sym;
                }
                pat->union_member_idx = -1; /* IT4: wildcard arm — no specific member */
                has_wildcard = true;
                Expr *body = elab_form(e, body_form);
                if (!body) { free(member_covered); return NULL; }
                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type;
                continue;
            }

            /* Type-narrowing pattern: (varname : TypeName)
             * The reader collapses ': TypeName' into a single F_TYPE_ANN node, so the
             * list has 2 items: [F_SYM varname, F_TYPE_ANN{inner}].
             * The 3-item form [F_SYM varname, F_SYM ":", F_SYM TypeName] is also
             * supported for defgadt-style bare colon separators. */
            bool is_type_narrowing_2 = (pat_form->tag == F_LIST &&
                pat_form->as.list.len == 2 &&
                pat_form->as.list.items[0]->tag == F_SYM &&
                pat_form->as.list.items[1]->tag == F_TYPE_ANN);
            bool is_type_narrowing_3 = (pat_form->tag == F_LIST &&
                pat_form->as.list.len == 3 &&
                pat_form->as.list.items[0]->tag == F_SYM &&
                pat_form->as.list.items[1]->tag == F_SYM &&
                pat_form->as.list.items[1]->as.sym == e->sym_colon);
            if (is_type_narrowing_2 || is_type_narrowing_3) {
                Form *var_form  = pat_form->as.list.items[0];
                Form *type_form = is_type_narrowing_2
                    ? pat_form->as.list.items[1]->as.list.items[0]  /* inner of F_TYPE_ANN */
                    : pat_form->as.list.items[2];
                /* Parse narrowed type */
                Type *narrowed = type_expr_from_form(e, type_form, NULL, NULL, NULL, 0);
                if (!narrowed) { free(member_covered); return NULL; }

                /* Find which union member this pattern covers */
                int covered_member = -1;
                for (uint8_t um = 0; um < n_members; um++) {
                    if (union_t->as.union_.members[um] &&
                        type_eq(*narrowed, *union_t->as.union_.members[um])) {
                        covered_member = (int)um;
                        break;
                    }
                }
                if (covered_member < 0) {
                    diag_emit_with_code(DIAG_ERROR, pat_form->span,
                                        TUR_E0300_UNION_TYPE_MISMATCH,
                                        "match: type '%s' is not a member of union '%s'",
                                        type_name(*narrowed), type_name(*union_t));
                    free(member_covered);
                    return NULL;
                }
                member_covered[covered_member] = true;
                pat->union_member_idx = covered_member; /* IT4: record for tag-dispatch in emit.c */

                /* Introduce arm scope with the narrowed binding */
                Scope arm_scope;
                scope_init(&arm_scope, e->scope);
                Scope *saved_scope = e->scope;
                e->scope = &arm_scope;

                Binding *var_b = binding_new(e, var_form->as.sym, *narrowed,
                                             false, false, var_form->span);
                scope_add(&arm_scope, var_b);

                Expr *body = elab_form(e, body_form);
                e->scope = saved_scope;
                if (!body) { free(member_covered); return NULL; }

                /* Record the binding in the pattern */
                pat->is_var = true;
                pat->var_sym = var_form->as.sym;
                pat->n_bindings = 1;
                pat->bindings = (Binding **)arena_alloc(e->arena, sizeof(Binding *));
                pat->bindings[0] = var_b;

                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type;
                continue;
            }

            /* Unrecognized pattern for union match */
            diag_emit(DIAG_ERROR, pat_form->span,
                      "match on union type: expected '(varname : Type)' or wildcard '_', got unexpected pattern");
            free(member_covered);
            return NULL;
        }

        /* Exhaustiveness check: every union member must be covered */
        if (!has_wildcard) {
            for (uint8_t um = 0; um < n_members; um++) {
                if (!member_covered[um] && union_t->as.union_.members[um]) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                                        TUR_E0301_NON_EXHAUSTIVE_UNION_MATCH,
                                        "match on union type '%s': missing arm for member '%s'",
                                        type_name(*union_t),
                                        type_name(*union_t->as.union_.members[um]));
                    free(member_covered);
                    return NULL;
                }
            }
        }
        free(member_covered);

        if (result_type.kind == TY_UNKNOWN) result_type = TYPE_NIL;
        Expr *out = expr_new(e->arena, EX_MATCH, result_type, call->span);
        out->as.match_.scrutinee = scrutinee;
        out->as.match_.arms = arms;
        out->as.match_.n_arms = n_arms;
        return out;
    }

    /* SS2: Session offer match — when scrutinee is TY_SESSION_OFFER, handle
     * Left/Right branch patterns.  The scrutinee carries the tag as int64_t
     * and keeps the channel pointer in val_exprs[0] for arm binding. */
    if (g_sessions_enabled && scrutinee->type.kind == TY_SESSION_OFFER) {
        MatchArm *arms = (MatchArm *)arena_alloc(e->arena, n_arms * sizeof(MatchArm));
        Type result_type = TYPE_UNKNOWN;
        const Symbol *sym_left  = intern_cstr(e->st, "Left");
        const Symbol *sym_right = intern_cstr(e->st, "Right");

        for (uint32_t ai = 0; ai < n_arms; ai++) {
            uint32_t base = arm_start[ai];
            Form *pat_form  = call->as.list.items[base];
            Form *body_form = call->as.list.items[arm_has_guard[ai] ? base + 3 : base + 1];
            MatchPattern *pat = &arms[ai].pattern;
            memset(pat, 0, sizeof(MatchPattern));
            arms[ai].guard = NULL;
            pat->union_member_idx = -1;

            if (pat_form->tag == F_SYM) {
                /* Wildcard */
                pat->is_wildcard = true;
                Expr *body = elab_form(e, body_form);
                if (!body) return NULL;
                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type;
                continue;
            }
            if (pat_form->tag == F_LIST && pat_form->as.list.len == 2 &&
                pat_form->as.list.items[0]->tag == F_SYM &&
                pat_form->as.list.items[1]->tag == F_SYM) {
                const Symbol *ctor_sym = pat_form->as.list.items[0]->as.sym;
                Form *var_form = pat_form->as.list.items[1];
                int arm_tag;
                Type arm_type;
                if (ctor_sym == sym_left) {
                    arm_tag = 0;
                    arm_type = scrutinee->type.as.session_.fst
                        ? *scrutinee->type.as.session_.fst : type_from_kind(TY_PTR_VOID);
                } else if (ctor_sym == sym_right) {
                    arm_tag = 1;
                    arm_type = scrutinee->type.as.session_.snd
                        ? *scrutinee->type.as.session_.snd : type_from_kind(TY_PTR_VOID);
                } else {
                    diag_emit(DIAG_ERROR, pat_form->span,
                              "session offer match: expected Left or Right pattern, got '%s'",
                              ctor_sym->name);
                    return NULL;
                }
                pat->union_member_idx = arm_tag;
                pat->n_bindings = 1;
                pat->bindings = (Binding **)arena_alloc(e->arena, sizeof(Binding *));
                Binding *fb = binding_new(e, var_form->as.sym, arm_type,
                                          false, false, var_form->span);
                if (arm_type.copy_kind == CK_LINEAR) fb->is_linear = true;
                pat->bindings[0] = fb;
                Scope arm_scope;
                scope_init(&arm_scope, e->scope);
                Scope *saved_scope = e->scope;
                e->scope = &arm_scope;
                scope_add(&arm_scope, fb);
                Expr *body = elab_form(e, body_form);
                e->scope = saved_scope;
                scope_free(&arm_scope);
                if (!body) return NULL;
                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type;
                continue;
            }
            diag_emit(DIAG_ERROR, pat_form->span,
                      "session offer match: expected (Left var) or (Right var) pattern");
            return NULL;
        }
        if (result_type.kind == TY_UNKNOWN) result_type = TYPE_NIL;
        Expr *out = expr_new(e->arena, EX_MATCH, result_type, call->span);
        out->as.match_.scrutinee = scrutinee;
        out->as.match_.arms = arms;
        out->as.match_.n_arms = n_arms;
        return out;
    }

    /* Phase S4-lit: Literal match — scrutinee is a primitive (non-ADT) type.
     * Patterns are literals (F_INT/F_BOOL/F_FLOAT/F_STR), _ wildcards, or
     * bare variable captures.  Emits EX_MATCH with is_literal arms. */
    {
        TypeKind _sk = scrutinee->type.kind;
        bool _is_prim = (_sk == TY_INT    || _sk == TY_BOOL   || _sk == TY_FLOAT  ||
                         _sk == TY_CSTR   || _sk == TY_INT8   || _sk == TY_INT16  ||
                         _sk == TY_INT32  || _sk == TY_INT64  || _sk == TY_UINT8  ||
                         _sk == TY_UINT16 || _sk == TY_UINT32 || _sk == TY_UINT64 ||
                         _sk == TY_FLOAT32 || _sk == TY_FLOAT64);
        /* Also trigger when first non-wildcard arm is a literal pattern */
        if (!_is_prim && (_sk == TY_UNKNOWN || _sk == TY_NIL)) {
            for (uint32_t _ai = 0; _ai < n_arms; _ai++) {
                FormTag _ft = call->as.list.items[arm_start[_ai]]->tag;
                if (_ft == F_INT || _ft == F_BOOL || _ft == F_FLOAT ||
                    _ft == F_STR || _ft == F_NIL) {
                    _is_prim = true;
                    break;
                }
                if (_ft != F_SYM) break; /* constructor found — not literal match */
            }
        }
        /* If any arm is an ADT constructor call (F_LIST), defer to the ADT path
         * even when the scrutinee type looks primitive (e.g. unannotated param
         * that defaulted to TY_INT — the ADT path infers the type from patterns). */
        if (_is_prim) {
            for (uint32_t _ai = 0; _ai < n_arms; _ai++) {
                if (call->as.list.items[arm_start[_ai]]->tag == F_LIST) {
                    _is_prim = false;
                    break;
                }
            }
        }
        if (_is_prim) {
            MatchArm *lit_arms = (MatchArm *)arena_alloc(e->arena, n_arms * sizeof(MatchArm));
            Type lit_result = TYPE_UNKNOWN;
            for (uint32_t ai = 0; ai < n_arms; ai++) {
                uint32_t base     = arm_start[ai];
                Form *pat_form    = call->as.list.items[base];
                Form *guard_raw   = arm_has_guard[ai] ? call->as.list.items[base + 2] : NULL;
                Form *body_form   = call->as.list.items[arm_has_guard[ai] ? base + 3 : base + 1];
                MatchPattern *pat = &lit_arms[ai].pattern;
                memset(pat, 0, sizeof(MatchPattern));
                pat->union_member_idx = -1;
                lit_arms[ai].guard = NULL;

                if (pat_form->tag == F_SYM) {
                    const Symbol *sym_wc = intern_cstr(e->st, "_");
                    if (pat_form->as.sym == sym_wc) {
                        pat->is_wildcard = true;
                    } else {
                        pat->is_var     = true;
                        pat->var_sym    = pat_form->as.sym;
                    }
                } else if (pat_form->tag == F_INT  || pat_form->tag == F_BOOL ||
                           pat_form->tag == F_FLOAT || pat_form->tag == F_STR  ||
                           pat_form->tag == F_NIL) {
                    pat->is_literal  = true;
                    pat->lit_kind    = (int8_t)pat_form->tag;
                    switch (pat_form->tag) {
                    case F_INT:   pat->lit_int   = pat_form->as.i; break;
                    case F_BOOL:  pat->lit_bool  = pat_form->as.b; break;
                    case F_FLOAT: pat->lit_float = pat_form->as.f; break;
                    case F_STR:   pat->lit_cstr  = pat_form->as.s.p; break;
                    default: break;
                    }
                } else {
                    diag_emit(DIAG_ERROR, pat_form->span,
                              "match: pattern must be a literal value, _ wildcard, "
                              "or variable capture when matching a primitive type");
                    return NULL;
                }

                /* Optional when-guard */
                if (guard_raw) {
                    lit_arms[ai].guard = elab_form(e, guard_raw);
                    if (!lit_arms[ai].guard) return NULL;
                }

                /* Elaborate body; for is_var, introduce the binding in a new scope */
                Expr *body;
                if (pat->is_var && pat->var_sym) {
                    Binding *vb = binding_new(e, pat->var_sym, scrutinee->type,
                                              false, false, pat_form->span);
                    pat->var_binding = vb;
                    Scope arm_sc;
                    scope_init(&arm_sc, e->scope);
                    Scope *saved_sc = e->scope;
                    e->scope = &arm_sc;
                    scope_add(&arm_sc, vb);
                    body = elab_form(e, body_form);
                    e->scope = saved_sc;
                    scope_free(&arm_sc);
                } else {
                    body = elab_form(e, body_form);
                }
                if (!body) return NULL;
                lit_arms[ai].body = body;
                if (lit_result.kind == TY_UNKNOWN) lit_result = body->type;
            }
            if (lit_result.kind == TY_UNKNOWN) lit_result = TYPE_NIL;
            Expr *out = expr_new(e->arena, EX_MATCH, lit_result, call->span);
            out->as.match_.scrutinee = scrutinee;
            out->as.match_.arms      = lit_arms;
            out->as.match_.n_arms    = n_arms;
            return out;
        }
    }

    /* If the scrutinee type is not already TY_ADT (e.g. an untyped param
     * that defaulted to TY_INT), or is TY_ADT with no def (e.g. return of
     * ADT-returning fn without result_full_type), infer the ADT from the
     * first constructor pattern in the arm list. */
    if (scrutinee->type.kind != TY_ADT || !scrutinee->type.as.adt_.def) {
        AdtDef *inferred_adt = NULL;
        for (uint32_t ai = 0; ai < n_arms && !inferred_adt; ai++) {
            Form *pat_f = call->as.list.items[arm_start[ai]];
            if (pat_f->tag == F_LIST && pat_f->as.list.len >= 1 &&
                pat_f->as.list.items[0]->tag == F_SYM) {
                CtorDef *cd = elab_lookup_ctor(e, pat_f->as.list.items[0]->as.sym);
                if (cd) inferred_adt = cd->adt;
            }
        }
        if (inferred_adt) {
            /* Patch the scrutinee type to the inferred ADT */
            scrutinee->type = type_adt(inferred_adt);
        } else {
            diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                      "match: scrutinee must be an ADT type, got %s",
                      typekind_to_string(scrutinee->type.kind));
            return NULL;
        }
    }

    AdtDef *adt = scrutinee->type.as.adt_.def;

    /* Allocate arms array */
    MatchArm *arms = (MatchArm *)arena_alloc(e->arena, n_arms * sizeof(MatchArm));

    /* Track covered constructors for exhaustiveness */
    bool *covered = (bool *)calloc(adt->n_ctors, sizeof(bool));
    bool has_wildcard = false;

    Type result_type = TYPE_UNKNOWN;

    /* LT1: Snapshot outer-scope linear consumed state before match arms.
     * We restore before each arm's body and verify consistency across arms at the end. */
    Binding **match_lin_bindings = NULL;
    bool *match_lin_before = NULL;
    uint32_t n_match_lin = 0;
    bool **arm_lin_states = NULL;
    bool *arm_diverges = NULL;
    if (g_linear_enabled) {
        n_match_lin = linear_state_snapshot_bindings(e->scope, &match_lin_bindings,
                                                     &match_lin_before);
        if (n_match_lin > 0) {
            arm_lin_states = (bool **)calloc(n_arms, sizeof(bool *));
            arm_diverges   = (bool  *)calloc(n_arms, sizeof(bool));
        }
    }

    for (uint32_t ai = 0; ai < n_arms; ai++) {
        uint32_t base = arm_start[ai];
        Form *pat_form = call->as.list.items[base];
        Form *guard_form_raw = arm_has_guard[ai]
            ? call->as.list.items[base + 2]
            : NULL;
        Form *body_form = call->as.list.items[arm_has_guard[ai] ? base + 3 : base + 1];

        MatchPattern *pat = &arms[ai].pattern;
        memset(pat, 0, sizeof(MatchPattern));
        arms[ai].guard = NULL;

        if (pat_form->tag == F_SYM) {
            /* Bare symbol: either _ wildcard or variable capture */
            const Symbol *sym = pat_form->as.sym;
            /* intern "_" */
            const Symbol *sym_wildcard = intern_cstr(e->st, "_");
            if (sym == sym_wildcard) {
                pat->is_wildcard = true;
                has_wildcard = true;
            } else {
                /* Variable binding — captures entire scrutinee */
                pat->is_var = true;
                pat->var_sym = sym;
                has_wildcard = true; /* covers all remaining */
            }
            /* No new scope needed; elaborate body directly */
            /* LT1: Restore outer linear state before this arm's body. */
            if (g_linear_enabled && n_match_lin > 0) {
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            Expr *body = elab_form(e, body_form);
            if (!body) { free(covered); return NULL; }
            /* LT1: Capture outer linear state after this arm's body. */
            if (g_linear_enabled && n_match_lin > 0) {
                arm_lin_states[ai] = linear_state_capture_current(match_lin_bindings, n_match_lin);
                arm_diverges[ai] = (body->type.kind == TY_NEVER) ||
                                   (body->kind == EX_RETURN) ||
                                   (body->kind == EX_PANIC)  ||
                                   (body->kind == EX_PANIC_WITH);
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            /* Phase G0: Arm body type consistency check for wildcard/variable arms. */
            if (result_type.kind != TY_UNKNOWN
                    && body->type.kind != TY_UNKNOWN
                    && !type_eq(result_type, body->type)) {
                diag_emit_with_code(DIAG_ERROR, body_form->span,
                                    TUR_E0001_TYPE_MISMATCH,
                                    "match: arm types are incompatible -- "
                                    "expected %s (from earlier arm), got %s",
                                    typekind_to_string(result_type.kind),
                                    typekind_to_string(body->type.kind));
                free(covered); return NULL;
            }
            arms[ai].body = body;
            if (result_type.kind == TY_UNKNOWN) result_type = body->type;
        } else if (pat_form->tag == F_LIST) {
            /* Constructor pattern: (CtorName var1 var2 ...) */
            if (pat_form->as.list.len < 1) {
                diag_emit(DIAG_ERROR, pat_form->span,
                          "match: empty constructor pattern");
                free(covered); return NULL;
            }
            Form *ctor_name_form = pat_form->as.list.items[0];
            if (ctor_name_form->tag != F_SYM) {
                diag_emit(DIAG_ERROR, ctor_name_form->span,
                          "match: constructor pattern must start with a constructor name");
                free(covered); return NULL;
            }
            const Symbol *ctor_sym = ctor_name_form->as.sym;
            /* Look up constructor in this ADT */
            CtorDef *ctor = NULL;
            for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
                if (strcmp(adt->ctors[ci]->name, ctor_sym->name) == 0) {
                    ctor = adt->ctors[ci];
                    break;
                }
            }
            if (!ctor) {
                diag_emit(DIAG_ERROR, ctor_name_form->span,
                          "match: '%s' is not a constructor of '%s'",
                          ctor_sym->name, adt->name);
                free(covered); return NULL;
            }

            uint32_t n_bindings = pat_form->as.list.len - 1;
            if (n_bindings != ctor->n_fields) {
                diag_emit(DIAG_ERROR, pat_form->span,
                          "match: constructor '%s' expects %u fields, got %u",
                          ctor->name, ctor->n_fields, n_bindings);
                free(covered); return NULL;
            }

            pat->ctor = ctor;
            pat->n_bindings = n_bindings;
            pat->bindings = n_bindings > 0
                ? (Binding **)arena_alloc(e->arena, n_bindings * sizeof(Binding *))
                : NULL;

            /* Phase G0: Redundant arm warning -- emit a warning if this constructor
             * was already covered by an earlier non-guarded arm.  We still
             * elaborate the body so any errors inside it are reported. */
            if (covered[ctor->tag]) {
                diag_emit(DIAG_WARNING, pat_form->span,
                          "match: arm for constructor '%s' is unreachable -- "
                          "already covered by an earlier arm",
                          ctor->name);
            }

            /* Phase G4: guarded arm doesn't guarantee coverage */
            covered[ctor->tag] = !arm_has_guard[ai];

            /* Phase G2: For GADT arms, build a per-arm SkolemEnv to resolve
             * type-variable field types to concrete kinds. */
            SkolemEnv arm_senv;
            arm_senv.n = 0;
            if (adt->is_gadt && ctor->result_type_form) {
                gadt_build_skolem_env(&arm_senv, adt, ctor);
            }
            SkolemEnv *saved_senv = e->g2_skolem_env;
            const CtorDef *saved_ctor = e->g2_current_ctor;
            if (adt->is_gadt) {
                e->g2_skolem_env = &arm_senv;
                e->g2_current_ctor = ctor;
            }

            /* Introduce a new scope with the field bindings */
            Scope arm_scope;
            scope_init(&arm_scope, e->scope);
            Scope *saved_scope = e->scope;
            e->scope = &arm_scope;

            for (uint32_t bi = 0; bi < n_bindings; bi++) {
                Form *var_form = pat_form->as.list.items[1 + bi];
                if (var_form->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, var_form->span,
                              "match: field binding must be a symbol");
                    e->scope = saved_scope;
                    scope_free(&arm_scope);
                    e->g2_skolem_env = saved_senv;
                    e->g2_current_ctor = saved_ctor;
                    free(covered); return NULL;
                }
                Type ftype;
                if (adt->is_gadt && ctor->field_forms && ctor->field_forms[bi]) {
                    /* Phase G2: resolve field type using the skolem env */
                    ftype = gadt_resolve_type_from_form(e, adt,
                                                        ctor->field_forms[bi], &arm_senv);
                } else if (ctor->field_forms && ctor->field_forms[bi]) {
                    /* F6-1 (cross-plan-followups): defdata ctor field with
                     * a stashed type form -- re-parse the type so the binding
                     * carries the declared ADT/struct, not just the C-level
                     * `int` collapsed by parse_struct_field_type for ADT-typed
                     * fields.  Falls back to type_from_kind below if the
                     * re-parse fails (e.g. unknown type). */
                    Type *resolved = type_expr_from_form(e,
                        (Form *)ctor->field_forms[bi],
                        NULL, NULL, NULL, 0);
                    if (resolved) {
                        ftype = *resolved;
                    } else {
                        ftype = type_from_kind(ctor->fields[bi].kind);
                    }
                } else {
                    ftype = type_from_kind(ctor->fields[bi].kind);
                }
                Binding *fb = binding_new(e, var_form->as.sym, ftype, false, false,
                                          var_form->span);
                /* LT1: Propagate linearity from the field's type to its binding */
                if (g_linear_enabled && ftype.copy_kind == CK_LINEAR) {
                    fb->is_linear = true;
                }
                scope_add(&arm_scope, fb);
                pat->bindings[bi] = fb;
            }

            /* LT1: Restore outer linear state before this arm's body. */
            if (g_linear_enabled && n_match_lin > 0) {
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            Expr *body = elab_form(e, body_form);

            /* Phase G4: Elaborate optional when-guard while arm scope is still live */
            Expr *guard_expr = NULL;
            if (arm_has_guard[ai] && guard_form_raw) {
                guard_expr = elab_form(e, guard_form_raw);
                if (!guard_expr) {
                    e->scope = saved_scope;
                    scope_free(&arm_scope);
                    e->g2_skolem_env = saved_senv;
                    e->g2_current_ctor = saved_ctor;
                    free(covered); return NULL;
                }
                if (guard_expr->type.kind != TY_BOOL) {
                    diag_emit(DIAG_ERROR, guard_form_raw->span,
                              "match: when-guard must have type bool, got %s",
                              typekind_to_string(guard_expr->type.kind));
                    e->scope = saved_scope;
                    scope_free(&arm_scope);
                    e->g2_skolem_env = saved_senv;
                    e->g2_current_ctor = saved_ctor;
                    free(covered); return NULL;
                }
            }

            e->scope = saved_scope;
            scope_free(&arm_scope);
            e->g2_skolem_env = saved_senv;
            e->g2_current_ctor = saved_ctor;
            /* LT1: Verify all linear field bindings in this arm were consumed */
            bool lt1_arm_fail = false;
            if (g_linear_enabled && body) {
                for (uint32_t bi2 = 0; bi2 < n_bindings; bi2++) {
                    Binding *fb2 = pat->bindings[bi2];
                    if (fb2->is_linear && !fb2->is_linear_consumed && !fb2->is_moved) {
                        diag_emit_with_code(DIAG_ERROR, fb2->span,
                                            TUR_E0100_LINEAR_DROPPED,
                                            "linear field '%s' dropped without being consumed in match arm",
                                            fb2->name->name);
                        lt1_arm_fail = true;
                    }
                }
            }
            /* ST1: Verify all relevant field bindings in this arm were used */
            bool st1_arm_fail = false;
            if (g_substructural_enabled && body) {
                for (uint32_t bi2 = 0; bi2 < n_bindings; bi2++) {
                    Binding *fb2 = pat->bindings[bi2];
                    if (fb2->is_relevant && fb2->usage_state == USAGE_UNUSED && !fb2->is_moved) {
                        diag_emit_with_code(DIAG_ERROR, fb2->span,
                                            TUR_E0151_RELEVANT_DROPPED,
                                            "relevant field '%s' dropped without being used in match arm",
                                            fb2->name->name);
                        st1_arm_fail = true;
                    }
                }
            }
            /* Phase G2: GADT constructor context -- when body elaboration fails
             * inside a GADT arm, emit a note naming the constructor whose
             * return-type annotation caused the type refinement and listing the
             * active skolem equalities (e.g. "in this arm: a ~ int").
             * This surfaces the "why" behind type errors that occur because of
             * GADT-induced substitutions. */
            if (!body && adt->is_gadt && arm_senv.n > 0) {
                char skolem_note[256];
                int pos = 0;
                for (uint8_t si = 0; si < arm_senv.n && pos < 230; si++) {
                    if (si > 0) pos += snprintf(skolem_note + pos,
                                                sizeof(skolem_note) - pos, ", ");
                    pos += snprintf(skolem_note + pos, sizeof(skolem_note) - pos,
                                   "%s ~ %s",
                                   arm_senv.bindings[si].name,
                                   typekind_to_string(arm_senv.bindings[si].kind));
                }
                skolem_note[pos] = '\0';
                diag_emit(DIAG_NOTE, body_form->span,
                          "inside arm for constructor '%s' of '%s'; "
                          "active refinements: %s",
                          ctor->name, adt->name, skolem_note);
            }
            if (!body || lt1_arm_fail || st1_arm_fail) { free(covered); return NULL; }

            /* Phase G2/HRT: detect skolem escape -- arm body result is anonymous TY_TYVAR.
             * Named TY_TYVAR (type variable from a :a parameter annotation) is a properly
             * polymorphic result and is allowed; only anonymous TY_TYVAR (unresolved field
             * type with no name) indicates a skolem that escaped its scope. */
            if (adt->is_gadt && body->type.kind == TY_TYVAR
                    && body->type.as.tyvar_.name == NULL) {
                diag_emit(DIAG_ERROR, body_form->span,
                          "match: skolem type variable escapes match arm "
                          "(constructor '%s' of '%s'): the arm body has an "
                          "unresolved GADT type variable as its result type",
                          ctor->name, adt->name);
                free(covered); return NULL;
            }

            /* Phase G0/G2: Arm body type consistency check.
             * All arms of a match expression must return the same type.
             * If this arm's body type differs from the type established by the
             * first arm, emit TUR_E0001_TYPE_MISMATCH.  For GADT arms, also
             * emit a note listing the active skolem equalities so the user can
             * see which type refinement is in effect. */
            if (result_type.kind != TY_UNKNOWN
                    && body->type.kind != TY_UNKNOWN
                    && !type_eq(result_type, body->type)) {
                if (adt->is_gadt && arm_senv.n > 0) {
                    char skolem_note[256];
                    int pos = 0;
                    for (uint8_t si = 0; si < arm_senv.n && pos < 230; si++) {
                        if (si > 0) pos += snprintf(skolem_note + pos,
                                                    sizeof(skolem_note) - pos, ", ");
                        pos += snprintf(skolem_note + pos, sizeof(skolem_note) - pos,
                                       "%s ~ %s",
                                       arm_senv.bindings[si].name,
                                       typekind_to_string(arm_senv.bindings[si].kind));
                    }
                    skolem_note[pos] = '\0';
                    diag_emit(DIAG_NOTE, body_form->span,
                              "inside arm for constructor '%s' of '%s'; "
                              "active refinements: %s",
                              ctor->name, adt->name, skolem_note);
                }
                diag_emit_with_code(DIAG_ERROR, body_form->span,
                                    TUR_E0001_TYPE_MISMATCH,
                                    "match: arm types are incompatible -- "
                                    "expected %s (from earlier arm), got %s",
                                    typekind_to_string(result_type.kind),
                                    typekind_to_string(body->type.kind));
                free(covered); return NULL;
            }

            arms[ai].body = body;
            arms[ai].guard = guard_expr;
            if (result_type.kind == TY_UNKNOWN) result_type = body->type;
            /* LT1: Capture outer linear state after this arm's body. */
            if (g_linear_enabled && n_match_lin > 0) {
                arm_lin_states[ai] = linear_state_capture_current(match_lin_bindings, n_match_lin);
                arm_diverges[ai] = (body->type.kind == TY_NEVER) ||
                                   (body->kind == EX_RETURN) ||
                                   (body->kind == EX_PANIC)  ||
                                   (body->kind == EX_PANIC_WITH);
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
        } else {
            diag_emit(DIAG_ERROR, pat_form->span,
                      "match: pattern must be a constructor list or _ wildcard");
            free(covered); return NULL;
        }
    }

    /* LT1: Verify consistent outer-scope linear consumption across match arms */
    if (g_linear_enabled && n_match_lin > 0 && arm_lin_states) {
        /* Find the first non-diverging arm as the reference. */
        int ref_ai = -1;
        for (uint32_t ai = 0; ai < n_arms; ai++) {
            if (arm_lin_states[ai] && !arm_diverges[ai]) {
                ref_ai = (int)ai; break;
            }
        }
        bool lin_ok = true;
        if (ref_ai >= 0) {
            for (uint32_t ai = (uint32_t)(ref_ai + 1); ai < n_arms; ai++) {
                if (!arm_lin_states[ai] || arm_diverges[ai]) continue;
                for (uint32_t li = 0; li < n_match_lin; li++) {
                    if (!match_lin_before[li] &&
                        arm_lin_states[ai][li] != arm_lin_states[ref_ai][li]) {
                        diag_emit_with_code(DIAG_ERROR, call->span,
                                            TUR_E0104_LINEAR_BRANCH_MISMATCH,
                                            "linear value '%s' consumed in some match arms but "
                                            "not others -- consume it in all arms or none",
                                            match_lin_bindings[li]->name->name);
                        lin_ok = false;
                    }
                }
            }
        }
        /* Merge: consumed after match only if all non-diverging arms consumed it. */
        for (uint32_t li = 0; li < n_match_lin; li++) {
            match_lin_bindings[li]->is_linear_consumed =
                (ref_ai >= 0) ? arm_lin_states[ref_ai][li] : false;
        }
        for (uint32_t ai = 0; ai < n_arms; ai++) free(arm_lin_states[ai]);
        free(arm_lin_states);
        free(arm_diverges);
        free(match_lin_before);
        free(match_lin_bindings);
        if (!lin_ok) { free(covered); return NULL; }
    }

    /* Exhaustiveness check */
    if (!has_wildcard) {
        /* Phase G2: For GADT ADTs, warn about unreachable constructors rather
         * than erroring — a constructor whose concrete type-argument instantiation
         * differs from every covered arm's instantiation is unreachable.
         * An uncovered constructor that IS reachable (or whose reachability is
         * unknown) is still an error. */
        if (adt->is_gadt) {
            /* Collect the first type-arg string from each covered arm's result type. */
            const char *covered_arg0[64]; /* max constructors we check */
            uint32_t n_covered = 0;
            for (uint32_t ci = 0; ci < adt->n_ctors && n_covered < 64; ci++) {
                if (!covered[ci]) continue;
                CtorDef *c = adt->ctors[ci];
                const char *a0 = NULL;
                if (c->result_type_form && c->result_type_form->tag == F_LIST
                    && c->result_type_form->as.list.len >= 2) {
                    Form *arg = c->result_type_form->as.list.items[1];
                    if (arg->tag == F_SYM) a0 = arg->as.sym->name;
                }
                covered_arg0[n_covered++] = a0;
            }
            for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
                if (covered[ci]) continue;
                CtorDef *c = adt->ctors[ci];
                /* Extract first type arg from this constructor's return type */
                const char *my_a0 = NULL;
                if (c->result_type_form && c->result_type_form->tag == F_LIST
                    && c->result_type_form->as.list.len >= 2) {
                    Form *arg = c->result_type_form->as.list.items[1];
                    if (arg->tag == F_SYM) my_a0 = arg->as.sym->name;
                }
                /* Check if my_a0 conflicts with all covered arms */
                bool all_covered_differ = (n_covered > 0) && (my_a0 != NULL);
                for (uint32_t k = 0; k < n_covered && all_covered_differ; k++) {
                    if (covered_arg0[k] == NULL ||
                        strcmp(covered_arg0[k], my_a0) == 0) {
                        all_covered_differ = false;
                    }
                }
                if (all_covered_differ) {
                    diag_emit(DIAG_WARNING, call->span,
                              "match: constructor '%s' of '%s' is unreachable for "
                              "this GADT instantiation",
                              c->name, adt->name);
                } else {
                    diag_emit(DIAG_ERROR, call->span,
                              "match: non-exhaustive patterns — constructor '%s' of '%s' not covered",
                              c->name, adt->name);
                    free(covered); return NULL;
                }
            }
        } else {
            for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
                if (!covered[ci]) {
                    diag_emit(DIAG_ERROR, call->span,
                              "match: non-exhaustive patterns — constructor '%s' of '%s' not covered",
                              adt->ctors[ci]->name, adt->name);
                    free(covered); return NULL;
                }
            }
        }
    }
    free(covered);

    if (result_type.kind == TY_UNKNOWN) result_type = TYPE_NIL;

    Expr *out = expr_new(e->arena, EX_MATCH, result_type, call->span);
    out->as.match_.scrutinee = scrutinee;
    out->as.match_.arms = arms;
    out->as.match_.n_arms = n_arms;
    return out;
}

/* Phase 11: make-struct - construct a struct value
 * Syntax: (make-struct StructName val1 val2 ...)
 * Returns a struct value (TY_STRUCT) with fields filled in positional order.
 */
Expr *elab_make_struct(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "make-struct requires a struct name: (make-struct StructName val1 ...)");
        return NULL;
    }

    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "make-struct: first argument must be a struct name");
        return NULL;
    }

    /* Look up the struct binding */
    Binding *struct_binding = scope_lookup(e->scope, name_form->as.sym);
    if (!struct_binding || struct_binding->type.kind != TY_STRUCT) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "make-struct: '%s' is not a defined struct type",
                  name_form->as.sym->name);
        return NULL;
    }

    StructDef *def = struct_binding->type.as.struct_.def;
    uint32_t n_given = call->as.list.len - 2; /* args after name */

    if (n_given != def->n_fields) {
        diag_emit(DIAG_ERROR, call->span,
                  "make-struct '%s': expected %u field value(s), got %u",
                  def->name, def->n_fields, n_given);
        return NULL;
    }

    /* Elaborate each field value */
    Expr **field_values = (Expr **)arena_alloc(e->arena, def->n_fields * sizeof(Expr *));
    for (uint32_t i = 0; i < def->n_fields; i++) {
        Expr *fv = elab_form(e, call->as.list.items[2 + i]);
        if (!fv) return NULL;
        field_values[i] = fv;

        /* Compound field annotations (TY_APP / TY_EXISTS / TY_FORALL) all
         * lower to TY_INT at the C level, so without this check a raw `42`
         * passed where `(exists [a] [(Show a)] a)` is expected slips through
         * and codegen reads the int as a `tur_existential_t *`.  TY_PTR_VOID
         * values are treated as a wildcard so inline-C escape hatches that
         * return an opaque pointer keep working. */
        if (def->fields[i].full_type) {
            Type expected = *def->fields[i].full_type;
            Type actual   = fv->type;
            if (actual.kind != TY_PTR_VOID && !type_eq(actual, expected)) {
                diag_emit(DIAG_ERROR, call->as.list.items[2 + i]->span,
                          "make-struct '%s': field '%s' expects %s, got %s",
                          def->name, def->fields[i].name,
                          type_name(expected), type_name(actual));
                return NULL;
            }
        }

        /* Move-at-make-struct for rc-managed payloads, mirroring the
         * F1-2-3 scan in elab_pack.  Ownership of an rc / weak / existential
         * reference transfers into the new struct field; the source binding
         * must not auto-drop at its enclosing scope's exit too. */
        if (fv->kind == EX_VAR && fv->as.var.binding) {
            TypeKind vk = fv->type.kind;
            if (vk == TY_RC || vk == TY_WEAK || vk == TY_EXISTS) {
                (void)binding_mark_moved(fv->as.var.binding,
                                         call->as.list.items[2 + i]->span);
            }
        }
    }

    /* Build the result type */
    Type result_type = type_struct(def);

    Expr *out = expr_new(e->arena, EX_MAKE_STRUCT, result_type, call->span);
    out->as.make_struct_.def = def;
    out->as.make_struct_.field_values = field_values;
    out->as.make_struct_.n_fields = def->n_fields;
    return out;
}

/* Elaborate (& expr) - create an immutable borrow
 * 
 * Syntax: (& expr)
 * Returns: &T where T is the type of expr
 * The borrow is valid for the duration of the enclosing scope.
 */
Expr *elab_borrow_immut(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "& takes exactly one argument: (& expr)");
        return NULL;
    }
    
    /* Elaborate the expression being borrowed */
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    
    /* Phase 12: Borrow tracking - check if inner is a binding and track the borrow */
    if (inner->kind == EX_VAR) {
        Binding *target = inner->as.var.binding;
        /* Check for use-after-move */
        if (target->is_moved) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0005_USE_AFTER_MOVE,
                                "cannot borrow `%s` because it was moved",
                                target->name->name);
            return NULL;
        }
        /* Check for borrow conflicts and add to active borrows */
        if (!scope_add_borrow(e->scope, target, BK_IMMUT, call->span)) {
            return NULL; /* Error already emitted */
        }
    }
    
    /* Phase U: Borrowing from ptr<void> requires an unsafe context. */
    if (inner->type.kind == TY_PTR_VOID && e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "cannot borrow from ptr<void> outside (unsafe ...)");
        return NULL;
    }

    /* Create the borrow type: &T where T is the referenced value's type.
     * Special cases:
     *   (& r) where r: ref<T>     → &T (borrow from owning ref)
     *   (& r) where r: &T or &mut T → &T (reborrow — same target type, not &&T)
     *   (& x) where x: T           → &T (plain borrow)
     */
    Type borrow_type;
    if (inner->type.kind == TY_REF) {
        borrow_type = type_ref_immut(inner->type.as.ref.inner);
    } else if (inner->type.kind == TY_REF_IMMUT || inner->type.kind == TY_REF_MUT) {
        borrow_type = type_ref_immut(inner->type.as.ref_borrow.target);
    } else {
        borrow_type = type_ref_immut(inner->type.kind);
    }
    
    /* Create the borrow expression */
    Expr *out = expr_new(e->arena, EX_BORROW_IMMUT, borrow_type, call->span);
    out->as.borrow_immut_.expr = inner;
    return out;
}

/* Elaborate (&mut expr) - create a mutable borrow
 * 
 * Syntax: (&mut expr)
 * Returns: &mut T where T is the type of expr
 * The borrow is valid for the duration of the enclosing scope.
 * Only one &mut T can exist for a given T at a time.
 */
Expr *elab_borrow_mut(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "&mut takes exactly one argument: (&mut expr)");
        return NULL;
    }
    
    /* Elaborate the expression being borrowed */
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    
    /* Phase 12: Borrow tracking - check if inner is a binding and track the borrow */
    if (inner->kind == EX_VAR) {
        Binding *target = inner->as.var.binding;
        /* Check for use-after-move */
        if (target->is_moved) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0005_USE_AFTER_MOVE,
                                "cannot mutably borrow `%s` because it was moved",
                                target->name->name);
            return NULL;
        }
        /* Check for borrow conflicts and add to active borrows */
        if (!scope_add_borrow(e->scope, target, BK_MUT, call->span)) {
            return NULL; /* Error already emitted */
        }
    }
    
    /* Phase U: Borrowing from ptr<void> requires an unsafe context. */
    if (inner->type.kind == TY_PTR_VOID && e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "cannot borrow from ptr<void> outside (unsafe ...)");
        return NULL;
    }

    /* Create the borrow type: &mut T where T is the referenced value's type.
     * Special cases:
     *   (&mut r) where r: ref<T>  → &mut T (mutable borrow from owning ref)
     *   (&mut r) where r: &mut T  → &mut T (mutable reborrow)
     *   (&mut r) where r: &T      → error (cannot take mutable borrow of immutable borrow)
     *   (&mut x) where x: T       → &mut T (plain mutable borrow)
     */
    Type borrow_type;
    if (inner->type.kind == TY_REF) {
        borrow_type = type_ref_mut(inner->type.as.ref.inner);
    } else if (inner->type.kind == TY_REF_IMMUT) {
        diag_emit(DIAG_ERROR, call->span,
                  "cannot borrow as mutable: source is already an immutable borrow `&T`");
        return NULL;
    } else if (inner->type.kind == TY_REF_MUT) {
        borrow_type = type_ref_mut(inner->type.as.ref_borrow.target);
    } else {
        borrow_type = type_ref_mut(inner->type.kind);
    }
    
    /* Create the borrow expression */
    Expr *out = expr_new(e->arena, EX_BORROW_MUT, borrow_type, call->span);
    out->as.borrow_mut_.expr = inner;
    return out;
}
