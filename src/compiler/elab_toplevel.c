/* elab_toplevel.c -- top-level form dispatch and the elaborate_program entry point. */
#include "elab_internal.h"

Expr *elab_as_cast(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'as' requires exactly two arguments: (as type expr)");
        return NULL;
    }
    Form *type_form = call->as.list.items[1];
    Form *expr_form = call->as.list.items[2];

    if (type_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "'as' expects a type name as first argument");
        return NULL;
    }
    TypeKind target_kind = typekind_from_symbol(type_form->as.sym->name);
    if (target_kind == TY_UNKNOWN) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "unknown type '%s' in 'as' cast", type_form->as.sym->name);
        return NULL;
    }
    if (!typekind_is_numeric(target_kind)) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "'as' casts are only valid for numeric types");
        return NULL;
    }

    Expr *inner = elab_form(e, expr_form);
    if (!inner) return NULL;

    if (!typekind_is_numeric(inner->type.kind)) {
        diag_emit(DIAG_ERROR, expr_form->span,
                  "cannot cast non-numeric type '%s' with 'as'",
                  type_name(inner->type));
        return NULL;
    }

    /* No-op cast: same type */
    if (inner->type.kind == target_kind) return inner;

    Type result_type = type_simple(target_kind, CK_COPY);
    Expr *out = expr_new(e->arena, EX_CAST, result_type, call->span);
    out->as.cast_.expr = inner;
    out->as.cast_.target_kind = target_kind;
    return out;
}

/* IT4: (type-of x) — return the cstr type name of an any-typed value. */
Expr *elab_any_type_of(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "'type-of' requires exactly one argument: (type-of x)");
        return NULL;
    }
    Expr *val = elab_form(e, call->as.list.items[1]);
    if (!val) return NULL;
    if (val->type.kind != TY_ANY) {
        diag_emit(DIAG_ERROR, call->span,
                  "'type-of' expects an 'any'-typed argument, got '%s'",
                  type_name(val->type));
        return NULL;
    }
    Type cstr_t = type_simple(TY_CSTR, CK_COPY);
    Expr *out = expr_new(e->arena, EX_ANY_TYPE_OF, cstr_t, call->span);
    out->as.any_type_of_.value = val;
    return out;
}

/* IT4: (cast x T) — unsafe downcast from any; returns the inner value unboxed as T.
 * T must be a primitive type name. No runtime tag check is emitted (unsafe). */
Expr *elab_any_cast(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'cast' requires exactly two arguments: (cast x T)");
        return NULL;
    }
    Expr *val = elab_form(e, call->as.list.items[1]);
    if (!val) return NULL;
    if (val->type.kind != TY_ANY) {
        diag_emit(DIAG_ERROR, call->span,
                  "'cast' expects an 'any'-typed first argument, got '%s'",
                  type_name(val->type));
        return NULL;
    }
    Form *type_form = call->as.list.items[2];
    if (type_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "'cast' expects a type name as second argument");
        return NULL;
    }
    TypeKind target_kind = typekind_from_symbol(type_form->as.sym->name);
    if (target_kind == TY_UNKNOWN) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "unknown type '%s' in 'cast'", type_form->as.sym->name);
        return NULL;
    }
    Type result_type = type_simple(target_kind, CK_COPY);
    Expr *out = expr_new(e->arena, EX_ANY_CAST, result_type, call->span);
    out->as.any_cast_.value = val;
    out->as.any_cast_.target_kind = target_kind;
    return out;
}

Expr *elab_form(Elab *e, Form *f) {
    switch (f->tag) {
        case F_NIL:  return e_nil(e, f->span);
        case F_BOOL: {
            Expr *out = expr_new(e->arena, EX_BOOL_LIT, TYPE_BOOL, f->span);
            out->as.b = f->as.b;
            return out;
        }
        case F_INT: {
            /* Phase N: dispatch to fixed-width type based on suffix */
            Type lit_type;
            switch (f->lit_suffix) {
                case LIT_SUF_I8:  lit_type = TYPE_INT8;    break;
                case LIT_SUF_I16: lit_type = TYPE_INT16;   break;
                case LIT_SUF_I32: lit_type = TYPE_INT32;   break;
                case LIT_SUF_I64: lit_type = TYPE_INT64;   break;
                case LIT_SUF_U8:  lit_type = TYPE_UINT8;   break;
                case LIT_SUF_U16: lit_type = TYPE_UINT16;  break;
                case LIT_SUF_U32: lit_type = TYPE_UINT32;  break;
                case LIT_SUF_U64: lit_type = TYPE_UINT64;  break;
                case LIT_SUF_F32: lit_type = TYPE_FLOAT32; break;
                case LIT_SUF_F64: lit_type = TYPE_FLOAT64; break;
                default:          lit_type = TYPE_INT;     break;
            }
            bool is_float_suffix = (f->lit_suffix == LIT_SUF_F32 || f->lit_suffix == LIT_SUF_F64);
            ExprKind ek = is_float_suffix ? EX_FLOAT_LIT : EX_INT_LIT;
            Expr *out = expr_new(e->arena, ek, lit_type, f->span);
            if (is_float_suffix) out->as.f = (double)f->as.i;
            else                 out->as.i = f->as.i;
            return out;
        }
        case F_FLOAT: {
            /* Phase N: dispatch float suffix */
            Type lit_type;
            switch (f->lit_suffix) {
                case LIT_SUF_F32: lit_type = TYPE_FLOAT32; break;
                case LIT_SUF_F64: lit_type = TYPE_FLOAT64; break;
                default:          lit_type = TYPE_FLOAT;   break;
            }
            Expr *out = expr_new(e->arena, EX_FLOAT_LIT, lit_type, f->span);
            out->as.f = f->as.f;
            return out;
        }
        case F_STR: {
            Expr *out = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, f->span);
            out->as.s = f->as.s;
            return out;
        }
        case F_KEYWORD:
            diag_emit(DIAG_ERROR, f->span,
                      "phase 1: keywords are only allowed as :else in cond or case");
            return NULL;
        case F_SYM: {
            /* M1: Use elab_lookup_sym for visibility + qualified name resolution */
            bool sym_qual_err = false;
            Binding *b = elab_lookup_sym(e, f->as.sym, f->span, &sym_qual_err);
            if (!b) {
                if (sym_qual_err) return NULL; /* error already emitted */
                /* Phase 8: Enhanced unbound symbol diagnostic with suggestions */
                const Symbol *best_match = NULL;
                int best_distance = 3;
                for (Scope *cur = e->scope; cur; cur = cur->parent) {
                    for (uint32_t i = 0; i < cur->n; i++) {
                        Binding *candidate = cur->bindings[i];
                        int dist = sym_levenshtein_distance(f->as.sym, candidate->name);
                        if (dist > 0 && dist < best_distance) {
                            best_distance = dist;
                            best_match = candidate->name;
                        }
                    }
                }
                if (best_match) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "unbound symbol '%s'", f->as.sym->name);
                    char sug_text[128];
                    snprintf(sug_text, sizeof(sug_text), "Did you mean '%s'?", best_match->name);
                    DiagSuggestion sug = {
                        sug_text,
                        NULL,
                        "https://turmeric-lang.dev/docs/errors/TUR-E0003"
                    };
                    diag_emit_with_suggestion(DIAG_ERROR, f->span, msg, &sug);
                } else {
                    diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0003_UNBOUND_SYMBOL,
                                        "unbound symbol '%s'", f->as.sym->name);
                }
                return NULL;
            }
            /* UT1: Check for use-after-consume of a unique binding (more specific than generic move) */
            if (g_unique_enabled && b->is_unique && b->is_moved) {
                diag_emit_with_code(DIAG_ERROR, f->span,
                                    TUR_E0201_UNIQUE_COPY,
                                    "cannot copy unique value '%s' -- "
                                    "unique values may be used at most once",
                                    b->name->name);
                return NULL;
            }
            /* Phase 11: Check for use-after-move */
            if (!binding_check_not_moved(b, f->span, "binding")) {
                return NULL;
            }
            /* LT1: Check for use-after-consume of a linear binding */
            if (g_linear_enabled && b->is_linear) {
                if (b->is_linear_consumed) {
                    diag_emit_with_code(DIAG_ERROR, f->span,
                                        TUR_E0101_LINEAR_USE_AFTER_CONSUME,
                                        "linear value '%s' used after being consumed",
                                        b->name->name);
                    return NULL;
                }
                /* Mark linear binding as consumed on use */
                b->is_linear_consumed = true;
            }
            /* ST1: Substructural usage tracking.
             * ^affine: may not be used more than once (TUR_E0150).
             * ^relevant: must be used at least once; duplicates are fine. */
            if (g_substructural_enabled) {
                if (b->is_affine && !b->is_continuation) {
                    /* Continuation affine checks are handled by cont_check_double_use
                     * (LC2) to emit continuation-specific error codes instead of E0150. */
                    if (b->usage_state >= USAGE_USED_ONCE) {
                        diag_emit_with_code(DIAG_ERROR, f->span,
                                            TUR_E0150_AFFINE_USED_TWICE,
                                            "affine value '%s' used more than once",
                                            b->name->name);
                        return NULL;
                    }
                    b->usage_state = USAGE_USED_ONCE;
                } else if (b->is_relevant && !b->is_continuation) {
                    /* Continuation relevant checks are handled by the LC2 drop check. */
                    b->usage_state = (b->usage_state == USAGE_UNUSED) ? USAGE_USED_ONCE
                                                                       : USAGE_USED_MANY;
                }
            }
            /* DV1: If the binding is a dynamic var, emit a dynvar-read node.
             * The result type is the var's value type, not TY_DYNVAR. */
            if (b->is_dynvar && b->dynvar_entry) {
                Expr *out = expr_new(e->arena, EX_DYNVAR_READ,
                                     b->dynvar_entry->value_type, f->span);
                out->as.dynvar_read_.entry = b->dynvar_entry;
                return out;
            }
            Expr *out = expr_new(e->arena, EX_VAR, b->type, f->span);
            out->as.var.binding = b;
            /* Phase HRT/G2: Resolve named type variable through current GADT match arm skolem env.
             * If a binding was declared as :a (TY_TYVAR with name "a"), and we are currently
             * inside a GADT match arm that has a skolem binding for "a", use the concrete type. */
            if (b->type.kind == TY_TYVAR && b->type.as.tyvar_.name != NULL && e->g2_skolem_env) {
                TypeKind resolved = gadt_skolem_lookup(e->g2_skolem_env, b->type.as.tyvar_.name);
                if (resolved != TY_UNKNOWN) {
                    out->type = type_from_kind(resolved);
                    out->type.copy_kind = typekind_default_copy_kind(resolved);
                }
            }
            return out;
        }
        case F_VEC:
            diag_emit(DIAG_ERROR, f->span,
                      "phase 1: vector literals are only allowed in let bindings");
            return NULL;
        case F_MAP:
            diag_emit(DIAG_ERROR, f->span,
                      "phase 1: map literals are parsed but not yet supported by elaboration");
            return NULL;
        case F_SET: {
            /* Phase X3: Elaborate set literal #s(e1 e2 ...) -> EX_SET_LIT */
            uint32_t n = f->as.list.len;
            Expr **items = arena_alloc(e->arena, n * sizeof(Expr *));
            for (uint32_t i = 0; i < n; i++) {
                items[i] = elab_form(e, f->as.list.items[i]);
                if (!items[i]) return NULL;
            }
            Type set_type = { .kind = TY_SET, .copy_kind = CK_COPY };
            Expr *ex = expr_new(e->arena, EX_SET_LIT, set_type, f->span);
            ex->as.set_lit_.items = items;
            ex->as.set_lit_.n = n;
            return ex;
        }
        /* Phase 6: quote form */
        case F_QUOTE: {
            /* (quote x) returns x as a literal without evaluating x */
            if (f->as.list.len != 1) {
                diag_emit(DIAG_ERROR, f->span,
                          "quote requires exactly one argument");
                return NULL;
            }
            Form *quoted = f->as.list.items[0];
            /* Quote just returns the inner form as a literal */
            /* For now, support quoting literals and symbols */
            return elab_form(e, quoted);
        }
        /* Phase 6: quasiquote forms - expand them */
        case F_QUASIQUOTE:
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            /* Expand quasiquote forms first */
            {
                Form *expanded = quasiquote_expand_form(e, f);
                return elab_form(e, expanded);
            }
        case F_CBLOCK: {
            /* Phase 2: inline C code block ```c ... ``` */
            /* U6: warn if inline-C appears outside an #{Unsafe}-annotated function */
            if (g_lint_inline_c_unsafe && e->unsafe_depth == 0) {
                diag_emit_with_code(DIAG_WARNING, f->span,
                    TUR_W0036_INLINE_C_MISSING_UNSAFE,
                    "inline-C block in function not annotated #{Unsafe}; "
                    "add #{Unsafe} to the function or wrap the call site in (unsafe ...)");
            }
            /* For now, we don't support captures, so the InlineC has no captures */
            InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
            ic->code = f->as.cblock;
            ic->return_type = TYPE_NIL; /* Will be inferred from context or default to void */
            ic->captures = NULL;
            ic->n_captures = 0;
            ic->val_exprs = NULL;
            ic->n_val_exprs = 0;
            
            Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_NIL, f->span);
            out->as.inline_c_.inline_c = ic;
            return out;
        }
        case F_LIST:
            if (f->as.list.len == 0) {
                diag_emit(DIAG_ERROR, f->span, "empty list ()");
                return NULL;
            }
            return elab_call(e, f);
        case F_TYPE_ANN:
            diag_emit(DIAG_ERROR, f->span,
                      "type annotation ': type' is only valid after a parameter name or as a return type");
            return NULL;
        /* CT0: Contract type annotations are not valid standalone expressions */
        case F_CONTRACT_TYPE:
            diag_emit(DIAG_ERROR, f->span,
                      "contract type '{ var : T | pred }' is only valid as a parameter or return type annotation");
            return NULL;
        /* RR3: Range literal variable annotation -- check for shadowing, then elaborate inner form. */
        case F_RANGE_VAR: {
            const Symbol *var_sym = f->as.list.items[0]->as.sym;
            Form *range_form = f->as.list.items[1];
            if (scope_lookup(e->scope, var_sym)) {
                diag_emit(DIAG_WARNING, f->span,
                          "#r{...}: variable '%s' shadows a binding in scope; "
                          "the name is not used in the expansion",
                          var_sym->name);
            }
            return elab_form(e, range_form);
        }
        /* INT-1: Reader conditional -- pick :tur or :turi branch based on g_interpret_mode */
        case F_READER_COND: {
            Form *tur_form = NULL, *turi_form = NULL;
            for (uint32_t i = 0; i + 1 < f->as.list.len; i += 2) {
                Form *key = f->as.list.items[i];
                Form *val = f->as.list.items[i + 1];
                if (key->tag == F_KEYWORD && strcmp(key->as.sym->name, "tur") == 0)
                    tur_form = val;
                else if (key->tag == F_KEYWORD && strcmp(key->as.sym->name, "turi") == 0)
                    turi_form = val;
            }
            Form *chosen = g_interpret_mode ? turi_form : tur_form;
            if (!chosen) return e_nil(e, f->span);
            return elab_form(e, chosen);
        }
    }
    return NULL;
}

Expr *elaborate_program(Arena *arena, SymbolTable *st,
                        Form *const *forms, uint32_t nforms,
                        uint32_t stdlib_prefix,
                        const char *module_base_dir,
                        bool separate_compilation,
                        bool sandboxed,
                        TypeClassEnv *out_tc_env,
                        const char **include_dirs,
                        int n_include_dirs,
                        uint32_t *out_n_file_scope_defs) {
    Elab e;
    elab_init_state(&e, arena, st);
    e.module_base_dir = module_base_dir ? module_base_dir : ".";
    /* stdlib fallback: TUR_STDLIB_DIR env var, else "stdlib" */
    {
        const char *sdir = getenv("TUR_STDLIB_DIR");
        e.module_stdlib_dir = (sdir && *sdir) ? sdir : "stdlib";
    }
    e.module_include_dirs   = include_dirs;
    e.n_module_include_dirs = n_include_dirs;
    e.separate_compilation  = separate_compilation;
    e.sandboxed             = sandboxed;
    builtins_init(st);

    int rc = 0;

    /* Phase M: (load "path") preprocessing.
     * Recursively expand all top-level (load "path") forms in place by
     * parsing the referenced file and splicing its forms into the form list.
     * Tracks visited paths to prevent duplicate loads and detect cycles.
     * Loaded forms then participate in the normal two-pass elaboration. */
    {
        const Symbol **loaded = NULL;
        uint32_t n_loaded = 0, cap_loaded = 0;
        Form **work = (Form **)arena_alloc(arena, nforms * sizeof(Form *));
        for (uint32_t i = 0; i < nforms; i++) work[i] = forms[i];
        uint32_t n_work = nforms;
        uint32_t cap_work = nforms;
        bool changed = true;
        while (changed) {
            changed = false;
            uint32_t out_n = 0;
            uint32_t out_cap = n_work + 16;
            Form **out = (Form **)arena_alloc(arena, out_cap * sizeof(Form *));
            for (uint32_t i = 0; i < n_work; i++) {
                Form *f = work[i];
                bool is_load = false;
                const Form *path_f = NULL;
                if (f->tag == F_LIST && f->as.list.len == 2 &&
                    f->as.list.items[0]->tag == F_SYM &&
                    f->as.list.items[0]->as.sym == e.sym_load &&
                    f->as.list.items[1]->tag == F_STR) {
                    is_load = true;
                    path_f = f->as.list.items[1];
                }
                if (!is_load) {
                    if (out_n >= out_cap) {
                        out_cap *= 2;
                        Form **nout = (Form **)arena_alloc(arena, out_cap * sizeof(Form *));
                        for (uint32_t k = 0; k < out_n; k++) nout[k] = out[k];
                        out = nout;
                    }
                    out[out_n++] = f;
                    continue;
                }
                /* (load "path") — read & parse */
                uint32_t plen = path_f->as.s.len;
                if (plen == 0 || plen >= 4096) {
                    diag_emit(DIAG_ERROR, path_f->span,
                              "load: path must be non-empty and < 4096 chars");
                    rc = -1;
                    continue;
                }
                char path_buf[4096];
                memcpy(path_buf, path_f->as.s.p, plen);
                path_buf[plen] = '\0';
                const Symbol *key = intern_cstr(st, path_buf);
                /* Already loaded? Skip silently (idempotent). */
                bool already = false;
                for (uint32_t k = 0; k < n_loaded; k++) {
                    if (loaded[k] == key) { already = true; break; }
                }
                if (already) continue;
                if (n_loaded >= cap_loaded) {
                    cap_loaded = cap_loaded ? cap_loaded * 2 : 8;
                    loaded = (const Symbol **)realloc((void *)loaded,
                              cap_loaded * sizeof(const Symbol *));
                    if (!loaded) { fprintf(stderr, "tur: oom\n"); abort(); }
                }
                loaded[n_loaded++] = key;
                /* Read source */
                char *src_raw = NULL;
                size_t src_len = 0;
                if (elab_read_file(path_buf, &src_raw, &src_len) != 0) {
                    diag_emit(DIAG_ERROR, path_f->span,
                              "load: cannot open '%s'", path_buf);
                    rc = -1;
                    continue;
                }
                char *src_copy = (char *)arena_alloc(arena, src_len + 1);
                memcpy(src_copy, src_raw, src_len);
                src_copy[src_len] = '\0';
                free(src_raw);
                char *path_copy = (char *)arena_alloc(arena, plen + 1);
                memcpy(path_copy, path_buf, plen + 1);
                SourceFile *sfile = (SourceFile *)arena_alloc(arena, sizeof(SourceFile));
                *sfile = (SourceFile){0};
                sfile->path = path_copy;
                sfile->src = src_copy;
                sfile->len = src_len;
                sfile->file_id = e.next_import_file_id++;
                sfile->reader_type = reader_type_from_extension(path_buf);
                if (sfile->reader_type == READER_UNKNOWN) sfile->reader_type = READER_TURMERIC;
                diag_register_file(sfile);
                uint32_t lf_n = 0;
                Form **lf = read_all(arena, st, sfile, &lf_n);
                if (!lf) { rc = -1; continue; }
                /* Splice loaded forms into output list */
                if (out_n + lf_n > out_cap) {
                    while (out_n + lf_n > out_cap) out_cap *= 2;
                    Form **nout = (Form **)arena_alloc(arena, out_cap * sizeof(Form *));
                    for (uint32_t k = 0; k < out_n; k++) nout[k] = out[k];
                    out = nout;
                }
                for (uint32_t k = 0; k < lf_n; k++) out[out_n++] = lf[k];
                changed = true; /* a new load may appear in loaded forms */
            }
            work = out;
            n_work = out_n;
            cap_work = out_cap;
            (void)cap_work;
        }
        free((void *)loaded);
        /* Replace forms/nforms with the expanded list for the rest of
         * elaborate_program.  Cast away const since we're in our own copy. */
        forms = (Form *const *)work;
        nforms = n_work;
    }

    Expr **items = (nforms == 0) ? NULL :
        (Expr **)arena_alloc(arena, nforms * sizeof(Expr *));

    /* Phase RF0: Type pre-pass -- pre-register all top-level defstruct and defdata
     * names as forward stubs BEFORE any elaboration, so that mutually recursive type
     * definitions can reference each other regardless of declaration order. */
    for (uint32_t i = 0; i < nforms; i++) {
        Form *f = forms[i];
        if (f->tag != F_LIST || f->as.list.len < 2) continue;
        Form *head = f->as.list.items[0];
        if (!head || head->tag != F_SYM) continue;
        bool is_defstruct = (head->as.sym == e.sym_defstruct);
        bool is_defdata   = (head->as.sym == e.sym_defdata);
        bool is_defgadt   = (head->as.sym == e.sym_defgadt);
        bool is_defopaque = (head->as.sym == e.sym_defopaque);
        if (!is_defstruct && !is_defdata && !is_defgadt && !is_defopaque) continue;
        /* GADTs are only registered if -Xgadt is enabled */
        if (is_defgadt && !g_gadt_enabled) continue;
        Form *name_f = f->as.list.items[1];
        if (!name_f || name_f->tag != F_SYM) continue;
        const Symbol *type_name = name_f->as.sym;
        /* Skip if already in scope (e.g. stdlib defines a type with this name) */
        if (scope_lookup(&e.global, type_name)) continue;
        /* Pre-allocate a stub def and register a forward binding */
        elab_add_forward_type(&e, type_name);
        if (is_defopaque) {
            StructDef *stub = (StructDef *)arena_alloc(arena, sizeof(StructDef));
            stub->name = type_name->name;
            stub->n_fields = 0;
            stub->fields = NULL;
            stub->is_copy = true;
            stub->is_linear = false;
            stub->needs_drop_glue = false;
            stub->is_opaque = true;
            stub->origin_file_id = name_f->span.file_id;
            /* Phase TM0: opaque types have no type params */
            stub->type_params = NULL;
            stub->n_type_params = 0;
            elab_register_struct_def(&e, stub);
            Type t = type_struct(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else if (is_defstruct) {
            StructDef *stub = (StructDef *)arena_alloc(arena, sizeof(StructDef));
            stub->name = type_name->name;
            stub->n_fields = 0;
            stub->fields = NULL;
            stub->is_copy = false;
            stub->is_linear = false;
            stub->needs_drop_glue = false;
            stub->origin_file_id = name_f->span.file_id;
            /* Phase TM0: initialize type_params for stub; filled in during elab_defstruct. */
            stub->type_params = NULL;
            stub->n_type_params = 0;
            elab_register_struct_def(&e, stub);
            Type t = type_struct(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else if (is_defgadt) {
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            stub->name = type_name->name;
            stub->n_ctors = 0;
            stub->ctors = NULL;
            stub->is_copy = false;
            stub->needs_drop_glue = false;
            stub->is_gadt = true;
            stub->type_params = NULL;
            stub->n_type_params = 0;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else {
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            stub->name = type_name->name;
            stub->n_ctors = 0;
            stub->ctors = NULL;
            stub->is_copy = false;
            stub->needs_drop_glue = false;
            stub->is_gadt = false;
            stub->type_params = NULL;
            stub->n_type_params = 0;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        }
    }

    /* Phase 2: Two-pass elaboration for mutual recursion support.
     * Pass 1: Collect all top-level defn declarations and add them to scope.
     * This allows mutually recursive functions to see each other. */
    for (uint32_t i = 0; i < nforms; i++) {
        Form *f = forms[i];
        if (f->tag == F_LIST && f->as.list.len > 0) {
            Form *head = f->as.list.items[0];
            if (head->tag == F_SYM) {
                if (head->as.sym == e.sym_defn) {
                    /* Parse defn declaration without body */
                    if (f->as.list.len >= 3) {
                        /* Phase R5: skip optional #[no-unwind] attribute */
                        uint32_t name_idx = 1;
                        if (f->as.list.items[1]->tag == F_SYM &&
                            f->as.list.items[1]->as.sym == e.sym_no_unwind_attr) {
                            name_idx = 2;
                        }
                        /* Phase M6: skip optional (export-as "c_name") attribute */
                        if ((uint32_t)f->as.list.len > name_idx &&
                            f->as.list.items[name_idx]->tag == F_LIST &&
                            f->as.list.items[name_idx]->as.list.len == 2 &&
                            f->as.list.items[name_idx]->as.list.items[0]->tag == F_SYM &&
                            f->as.list.items[name_idx]->as.list.items[0]->as.sym == e.sym_export_as_attr) {
                            name_idx += 1; /* skip (export-as "c_name") */
                        }
                        if ((uint32_t)f->as.list.len <= name_idx) goto next_form;
                        Form *name_f = f->as.list.items[name_idx];
                        if (name_f->tag == F_SYM) {
                            /* Parse return type annotation if present */
                            TypeKind return_kind = TY_INT; /* default */
                            uint32_t ret_idx = name_idx + 2; /* name params :ret */
                            if (f->as.list.len > ret_idx) {
                                Form *ret_f = f->as.list.items[ret_idx];
                                if (ret_f->tag == F_KEYWORD) {
                                    const Symbol *kw = ret_f->as.sym;
                                    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                                        return_kind = TY_INT;
                                    } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                                        return_kind = TY_BOOL;
                                    } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                                        return_kind = TY_NIL;
                                    } else if (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0) {
                                        /* SS3a: :nil return type must forward-declare as void,
                                         * not TY_INT, so recursive nil-returning functions
                                         * correctly infer TY_NIL for their body type. */
                                        return_kind = TY_NIL;
                                    } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                                        return_kind = TY_CSTR;
                                    } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                                        return_kind = TY_PTR_VOID;
                                    } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                                        return_kind = TY_PTR_VOID;
                                    }
                                } else if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len > 0) {
                                    /* Compound return type: peek at the head symbol to
                                     * recognize Session[P] returns for pass-1 forward decls. */
                                    Form *head_f = ret_f->as.list.items[0];
                                    if (head_f->tag == F_SYM &&
                                            strcmp(head_f->as.sym->name, "Session") == 0) {
                                        return_kind = TY_SESSION;
                                    }
                                }
                            }
                            /* Create a forward function type with 1 int param and parsed return type */
                            TypeKind arg_kinds[MAX_FN_ARITY] = {TY_INT};
                            Type fn_type = type_fn(arg_kinds, 1, return_kind);
                            Binding *b = binding_new(&e, name_f->as.sym, fn_type, false, true, f->span);
                            scope_add(&e.global, b);
                        }
                    }
                    next_form:;
                }
            }
        }
    }

    /* Phase M0: Validate defmodule position — must be the first user form */
    for (uint32_t i = stdlib_prefix; i < nforms; i++) {
        Form *f = forms[i];
        if (f->tag == F_LIST && f->as.list.len > 0) {
            Form *head = f->as.list.items[0];
            if (head->tag == F_SYM && head->as.sym == e.sym_defmodule) {
                if (i != stdlib_prefix) {
                    diag_emit(DIAG_ERROR, head->span,
                              "defmodule must be the first form in the file");
                    diag_emit(DIAG_NOTE, forms[stdlib_prefix]->span,
                              "this form comes before defmodule; move it inside the defmodule body or below it");
                    rc = -1;
                }
                break; /* only check the first occurrence */
            }
        }
    }
    if (rc != 0) {
        scope_free(&e.global);
        free(e.struct_defs);
        free(e.adt_defs);
        free(e.forward_type_syms);
        free(e.handled_effect_names);
        free(e.macros);
        return NULL;
    }

    /* Pass 2: Elaborate all forms */
    for (uint32_t i = 0; i < nforms; i++) {
        items[i] = elab_form(&e, forms[i]);
        if (!items[i]) { rc = -1; /* keep going to surface more diagnostics */ }

        /* Phase M7: Each auto-loaded stdlib file is conceptually its own file,
         * so reset has_defmodule after each stdlib defmodule. Otherwise the
         * second stdlib (safe.tur after macros.tur) trips the "one defmodule
         * per file" check. */
        if (i < stdlib_prefix && items[i] && items[i]->kind == EX_DEFMODULE) {
            e.has_defmodule = false;
        }

        /* Phase M7: Promote auto-loaded stdlib module exports back to
         * "stdlib pre-module" status (defining_module_name = NULL) so they
         * remain globally visible from user code without explicit import.
         * Triggered after the last stdlib form has been processed. */
        if (i + 1 == stdlib_prefix && stdlib_prefix > 0) {
            for (uint32_t k = 0; k < e.global.n; k++) {
                Binding *gb = e.global.bindings[k];
                if (gb->defining_module_name != NULL &&
                    gb->defining_module_name->len >= 4 &&
                    memcmp(gb->defining_module_name->name, "tur/", 4) == 0) {
                    gb->defining_module_name = NULL;
                }
            }
            for (uint32_t k = 0; k < e.n_macros; k++) {
                MacroDef *m = e.macros[k];
                if (m->defining_module_name != NULL &&
                    m->defining_module_name->len >= 4 &&
                    memcmp(m->defining_module_name->name, "tur/", 4) == 0) {
                    m->defining_module_name = NULL;
                }
            }
        }
    }

    /* Phase 3: Prepend file-scope definitions (from nested fn) */
    if (e.n_file_scope_defs > 0) {
        /* Allocate new items array with room for file-scope defs */
        Expr **new_items = (Expr **)arena_alloc(arena, 
            (nforms + e.n_file_scope_defs) * sizeof(Expr *));
        /* Copy file-scope defs first */
        for (uint32_t i = 0; i < e.n_file_scope_defs; i++) {
            new_items[i] = e.file_scope_defs[i];
        }
        /* Copy original items */
        for (uint32_t i = 0; i < nforms; i++) {
            new_items[e.n_file_scope_defs + i] = items[i];
        }
        items = new_items;
        nforms += e.n_file_scope_defs;
        /* Free the malloc'd file_scope_defs array */
        free(e.file_scope_defs);
    }

    /* Phase M6: Check for C symbol name collisions among exported bindings.
     * Two exported bindings from different modules collide when their mangled
     * C names are identical (e.g. module "my-lib" and "my_lib" both exporting
     * "foo" would both produce "my_lib__foo"). */
    if (rc == 0) {
        uint32_t n_exp = 0;
        for (uint32_t i = 0; i < e.global.n; i++) {
            if (e.global.bindings[i]->is_exported &&
                e.global.bindings[i]->defining_module_name != NULL)
                n_exp++;
        }
        if (n_exp > 1) {
            char **mangled = (char **)malloc(n_exp * sizeof(char *));
            Binding **exp_bindings = (Binding **)malloc(n_exp * sizeof(Binding *));
            if (!mangled || !exp_bindings) { fprintf(stderr, "tur: oom\n"); abort(); }
            uint32_t idx = 0;
            for (uint32_t i = 0; i < e.global.n; i++) {
                Binding *b = e.global.bindings[i];
                if (b->is_exported && b->defining_module_name != NULL) {
                    mangled[idx] = elab_mangle_binding_name(b);
                    exp_bindings[idx] = b;
                    idx++;
                }
            }
            for (uint32_t i = 0; i < n_exp; i++) {
                for (uint32_t j = i + 1; j < n_exp; j++) {
                    if (exp_bindings[i] == exp_bindings[j]) continue;
                    if (strcmp(mangled[i], mangled[j]) == 0) {
                        diag_emit(DIAG_ERROR, exp_bindings[j]->span,
                                  "exported symbol '%s' from module '%s' mangles to "
                                  "the same C name '%s' as '%s' from module '%s'; "
                                  "rename one or use (export-as \"...\") to assign a unique C name",
                                  exp_bindings[j]->name->name,
                                  exp_bindings[j]->defining_module_name->name,
                                  mangled[j],
                                  exp_bindings[i]->name->name,
                                  exp_bindings[i]->defining_module_name->name);
                        rc = -1;
                    }
                }
            }
            for (uint32_t i = 0; i < n_exp; i++) free(mangled[i]);
            free(mangled);
            free(exp_bindings);
        }
    }

    scope_free(&e.global);
    free(e.struct_defs);
    free(e.adt_defs);
    free(e.forward_type_syms);
    free(e.handled_effect_names);
    free(e.macros);
    free(e.loaded_modules); /* Phase M2 */
    if (rc != 0) return NULL;

    Expr *prog = expr_new(arena, EX_PROGRAM, TYPE_NIL,
                          nforms > 0 ? forms[0]->span : (Span){0,0,0,0,0,0});
    prog->as.program.items = items;
    prog->as.program.n = nforms;
    /* CPS-CL10: expose typeclass env to callers (e.g. cps_transform) */
    if (out_tc_env) *out_tc_env = e.typeclass_env;
    /* Tier 3: expose actual file-scope-def count so the interpreter can
     * distinguish them from (load ...)-expanded inline forms. */
    if (out_n_file_scope_defs)
        *out_n_file_scope_defs = (uint32_t)e.n_file_scope_defs;
    return prog;
}
