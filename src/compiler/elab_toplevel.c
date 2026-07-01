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

/* IT4/TY2.3: (cast x T) — checked downcast from any.  Verifies the runtime box
 * tag matches T's TypeKind and panics on mismatch (see __tur_any_cast_check),
 * then returns the inner value as T.  T may be a primitive type name, or a
 * struct/ADT name (TY2.2 heap-boxed payloads unbox by dereference). */
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
    Type result_type;
    if (target_kind == TY_UNKNOWN) {
        /* TY2.2: a struct/ADT name is a valid cast target. */
        Type *named = elab_lookup_type_by_name(e, type_form->as.sym);
        if (!named) {
            diag_emit(DIAG_ERROR, type_form->span,
                      "unknown type '%s' in 'cast'", type_form->as.sym->name);
            return NULL;
        }
        /* CONV-S1: a struct-origin lowered ADT casts under its box tag, keeping
         * the cast transparent to the defstruct-as-defadt lowering. */
        target_kind = any_box_tag_for_type(named);
        result_type = *named;
    } else {
        result_type = type_simple(target_kind, CK_COPY);
    }
    Expr *out = expr_new(e->arena, EX_ANY_CAST, result_type, call->span);
    out->as.any_cast_.value = val;
    out->as.any_cast_.target_kind = target_kind;
    /* structdef-retirement DS-C: no StructDef target -- lowered structs are ADTs. */
    out->as.any_cast_.target_struct = NULL;
    return out;
}

/* TY3: (is? x T) — runtime type test on an `any`-typed value.  Returns bool:
 * true iff x's stored box tag is T's TypeKind.  T may be a primitive, struct,
 * or ADT name.  Used directly, or as an `if` guard that narrows x to T. */
Expr *elab_is_q(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'is?' requires exactly two arguments: (is? x T)");
        return NULL;
    }
    Expr *val = elab_form(e, call->as.list.items[1]);
    if (!val) return NULL;
    if (val->type.kind != TY_ANY) {
        diag_emit(DIAG_ERROR, call->span,
                  "'is?' expects an 'any'-typed first argument, got '%s'",
                  type_name(val->type));
        return NULL;
    }
    Form *type_form = call->as.list.items[2];
    if (type_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "'is?' expects a type name as second argument");
        return NULL;
    }
    TypeKind test_kind = typekind_from_symbol(type_form->as.sym->name);
    if (test_kind == TY_UNKNOWN) {
        Type *named = elab_lookup_type_by_name(e, type_form->as.sym);
        if (!named) {
            diag_emit(DIAG_ERROR, type_form->span,
                      "unknown type '%s' in 'is?'", type_form->as.sym->name);
            return NULL;
        }
        /* CONV-S1: struct-origin lowered ADT tests as TY_STRUCT, matching the
         * box tag set by elab_coerce_to_any. */
        test_kind = any_box_tag_for_type(named);
    }
    Type bool_t = type_simple(TY_BOOL, CK_COPY);
    Expr *out = expr_new(e->arena, EX_ANY_IS, bool_t, call->span);
    out->as.any_is_.value = val;
    out->as.any_is_.test_tag = (int64_t)test_kind;
    return out;
}

/* DL1: build a synthetic (head arg0 arg1 ...) call form for a data literal. */
static Form *dl_build_call(Elab *e, Span span, const char *head,
                           Form **items, uint32_t n) {
    const Symbol *h = symtab_intern(e->st, strslice(head, (uint32_t)strlen(head)));
    Form **call_items = (Form **)arena_alloc(e->arena, (n + 1) * sizeof(Form *));
    call_items[0] = form_sym(e->arena, span, h);
    for (uint32_t i = 0; i < n; i++) call_items[i + 1] = items[i];
    return form_list(e->arena, span, call_items, n + 1);
}

/* DL1: normalize a #map{...} key form to the int key the typed Map expects.
 * Int keys pass through; keyword/string keys lower to (hamt/hash-str "name")
 * so equal keyword/string keys hash identically (content equality).  The
 * reader (TUR-E0282) guarantees the key is one of these three forms. */
static Form *dl_normalize_map_key(Elab *e, Form *key) {
    if (key->tag == F_INT) return key;
    /* SYM3 (runtime-symbols-plan): a keyword key is a first-class :Sym value
     * -- pass the F_KEYWORD through so the map is keyed by Sym
     * (pointer-identity, via Hash[Sym] / MapKey[Sym]) instead of decaying to a
     * content-hashed cstr. */
    if (key->tag == F_KEYWORD) return key;
    Form *str;
    if (key->tag == F_KEYWORD) {
        str = form_str(e->arena, key->span, key->as.sym->name,
                       (uint32_t)key->as.sym->len);
    } else { /* F_STR */
        str = form_str(e->arena, key->span, key->as.s.p, key->as.s.len);
    }
    Form *items[1] = { str };
    return dl_build_call(e, key->span, "hamt/hash-str", items, 1);
}

/* inline-c-cname-module-prefix-plan (Option A): resolve __TUR_CNAME_<name>__
 * splices in an inline-C body at elaboration time.
 *
 * The bare __TUR_CNAME_ emit-time path is mangle-only -- it reproduces the
 * name-mangling scheme but cannot reproduce the *module prefix* a global
 * defined inside a named module carries in its C name (raw_name_for_binding).
 * To make the splice resolve to the callee's exact emitted C name (prefix +
 * any (export-as ...) alias), we resolve each name here through the same
 * elab_lookup_sym path ordinary calls use, attach the resolved Binding to the
 * node's captures[], and rewrite the placeholder to the corresponding
 * __TUR_CAP_N__ -- which the emitter already lowers via name_for_binding
 * (prefix + alias for free).
 *
 * Names that do NOT resolve in scope are left as __TUR_CNAME_<name>__ verbatim
 * and fall through to the emit-time mangle-only path. This preserves the
 * established escape hatch for referencing an unprefixed global in another
 * translation unit that the current module does not import (e.g. stdlib
 * carrier helpers referenced across files without an explicit import).
 *
 * Returns the (possibly rewritten) code slice; on resolution it allocates the
 * captures array from the arena and writes it back through the out params. */
static StrSlice elab_cblock_resolve_cnames(Elab *e, StrSlice code, Span span,
                                           Binding ***out_caps, uint8_t *out_n) {
    *out_caps = NULL;
    *out_n = 0;
    const char *src = code.p;
    uint32_t len = code.len;
    if (!src || len < 14) return code;

    /* First pass: does the body contain any resolvable __TUR_CNAME_ splice?
     * If not, return the slice untouched (no copy, no captures). */
    Binding *caps[255];
    uint8_t n_caps = 0;
    bool any_rewrite = false;
    for (uint32_t i = 0; i + 14 <= len; ) {
        if (memcmp(src + i, "__TUR_CNAME_", 12) != 0) { i++; continue; }
        uint32_t name_start = i + 12;
        uint32_t j = name_start;
        while (j + 1 < len && !(src[j] == '_' && src[j + 1] == '_')) j++;
        if (!(j + 1 < len && src[j] == '_' && src[j + 1] == '_' && j > name_start)) {
            i = name_start; continue;
        }
        uint32_t name_len = j - name_start;
        const Symbol *sym = symtab_intern(e->st, strslice(src + name_start, name_len));
        bool qual_err = false;
        Binding *b = elab_lookup_sym(e, sym, span, &qual_err);
        if (b && n_caps < 255) {
            /* Dedup so a name used N times shares one capture slot. */
            bool seen = false;
            for (uint8_t k = 0; k < n_caps; k++) {
                if (caps[k] == b) { seen = true; break; }
            }
            if (!seen) caps[n_caps++] = b;
            any_rewrite = true;
        }
        i = j + 2;
    }
    if (!any_rewrite) return code;

    /* Second pass: rebuild the body, substituting resolvable splices with
     * their capture placeholder and leaving the rest byte-for-byte. */
    Buf out; buf_init(&out);
    for (uint32_t i = 0; i < len; ) {
        if (i + 14 <= len && memcmp(src + i, "__TUR_CNAME_", 12) == 0) {
            uint32_t name_start = i + 12;
            uint32_t j = name_start;
            while (j + 1 < len && !(src[j] == '_' && src[j + 1] == '_')) j++;
            if (j + 1 < len && src[j] == '_' && src[j + 1] == '_' && j > name_start) {
                uint32_t name_len = j - name_start;
                const Symbol *sym = symtab_intern(e->st,
                                                  strslice(src + name_start, name_len));
                bool qual_err = false;
                Binding *b = elab_lookup_sym(e, sym, span, &qual_err);
                int cap_idx = -1;
                if (b) {
                    for (uint8_t k = 0; k < n_caps; k++) {
                        if (caps[k] == b) { cap_idx = k; break; }
                    }
                }
                if (cap_idx >= 0) {
                    buf_printf(&out, "__TUR_CAP_%d__", cap_idx);
                    i = j + 2;
                    continue;
                }
                /* Unresolved: copy the splice verbatim for the emit-time
                 * mangle-only fallback. */
                buf_write(&out, src + i, (size_t)(j + 2 - i));
                i = j + 2;
                continue;
            }
        }
        buf_putc(&out, src[i++]);
    }

    char *copied = arena_strdup(e->arena, out.data, out.len);
    uint32_t new_len = (uint32_t)out.len;
    buf_free(&out);

    Binding **caps_arena = (Binding **)arena_alloc(e->arena,
                                                   n_caps * sizeof(Binding *));
    for (uint8_t k = 0; k < n_caps; k++) caps_arena[k] = caps[k];
    *out_caps = caps_arena;
    *out_n = n_caps;
    return strslice(copied, new_len);
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
            /* SYM0 (runtime-symbols-plan): a keyword in expression position is a
             * first-class :Sym literal whose runtime value is a
             * pointer-identity-equal interned symbol.  (Its other uses are
             * syntactic: type annotations, :refer/:as, struct-field selectors,
             * ADT tags -- all consumed by earlier passes before elab_form.) */
            {
                Expr *out = expr_new(e->arena, EX_SYM_LIT, TYPE_SYM, f->span);
                out->as.sym_lit_.sym = f->as.sym;
                return out;
            }
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
            if (b->is_unique && b->is_moved) {
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
            if (b->is_linear) {
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
            /* DL1: in expression position (i.e. not consumed structurally by a
             * binding form), a [...] vector lowers to (vec-of ...).  Binding
             * forms (defn/fn/let/loop/...) grab their F_VEC slot before it ever
             * reaches elab_form, so reaching here means expression position. */
            {
                Form *call = dl_build_call(e, f->span, "vec-of",
                                           f->as.list.items, f->as.list.len);
                return elab_form(e, call);
            }
        case F_MAP:
            diag_emit(DIAG_ERROR, f->span,
                      "phase 1: map literals are parsed but not yet supported by elaboration");
            return NULL;
        /* DL1: data literals lower to their stdlib constructor macros. */
        case F_MAP_LITERAL: {
            uint32_t n = f->as.list.len; /* even -- validated by reader */
            /* TMS3 (typed-map-surface-plan): every #map{...} literal lowers to
             * the single typed hamt-of builder, which dispatches by (Hash K) +
             * (MapKey K).  String keys pass through raw -- map-assoc hashes and
             * compares them by content via Hash[cstr]/MapKey[cstr], so distinct
             * pointers with equal text collapse to one key (no smap-of split).
             * Keyword keys are still hash-normalized to an int; int keys pass
             * through, zero overhead. */
            bool all_str_keys = (n > 0);
            for (uint32_t i = 0; i + 1 < n; i += 2) {
                if (f->as.list.items[i]->tag != F_STR) { all_str_keys = false; break; }
            }
            Form **kvs = (n == 0) ? NULL
                : (Form **)arena_alloc(e->arena, n * sizeof(Form *));
            for (uint32_t i = 0; i + 1 < n; i += 2) {
                /* String keys stay raw (content-keyed by map-assoc); other key
                 * literals (keywords) are hash-normalized to their int key. */
                kvs[i]     = all_str_keys ? f->as.list.items[i]
                                          : dl_normalize_map_key(e, f->as.list.items[i]);
                kvs[i + 1] = f->as.list.items[i + 1];
            }
            Form *call = dl_build_call(e, f->span, "hamt-of", kvs, n);
            return elab_form(e, call);
        }
        case F_SET_LITERAL: {
            Form *call = dl_build_call(e, f->span, "set-of",
                                       f->as.list.items, f->as.list.len);
            return elab_form(e, call);
        }
        /* Variadic HKT rows: a #row{...} type-row is a TYPE, not a value. It is
         * only meaningful in type-annotation position (where type_expr_from_form
         * lowers it to a TY_TYPEROW). Reaching the value elaborator means it was
         * written where an expression is expected. */
        case F_ROW_LITERAL:
            diag_emit(DIAG_ERROR, f->span,
                      "#row{...} is a type-level row and can only appear in a "
                      "type annotation, not as a value expression");
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
            /* Quoting a bare symbol yields a first-class :Sym literal --
             * the same lowering the F_KEYWORD branch below uses for
             * `:foo`. This lets DSL helpers in defns construct AST
             * nodes without TUR-E0003 chasing the inner symbol against
             * scope. See
             * docs/reported/defgodot-script-macro-vec-quote-semantics.md. */
            if (quoted->tag == F_SYM) {
                Expr *out = expr_new(e->arena, EX_SYM_LIT, TYPE_SYM, f->span);
                out->as.sym_lit_.sym = quoted->as.sym;
                return out;
            }
            /* Non-symbol quoted forms (literals, lists): fall back to
             * the legacy "elaborate as expression" behaviour. Runtime
             * list-of-values construction for `(quote (a b c))` is a
             * follow-up. */
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
            /* Hoist regex.h to the file preamble when any inline-C
             * references it. Per-function `#include <regex.h>` only takes
             * effect for the first function in the TU (subsequent includes
             * are suppressed by the header guard), so without hoisting,
             * multi-function stdlib modules like stdlib/re.tur fail to
             * compile. */
            extern bool g_needs_regex_h;
            if (!g_needs_regex_h && f->as.cblock.p) {
                const char *needle = "regex.h";
                size_t nlen = 7;
                if (f->as.cblock.len >= nlen) {
                    for (uint32_t i = 0; i + nlen <= f->as.cblock.len; ++i) {
                        if (memcmp(f->as.cblock.p + i, needle, nlen) == 0) {
                            g_needs_regex_h = true;
                            break;
                        }
                    }
                }
            }
            /* inline-c-function-scope-include-guards fix: pre-populate the
             * hoisted-include set during elaboration so emit_module can
             * write the directives at file scope before any function body
             * is emitted. The same scan re-runs at emit time inside
             * inline_c_substitute to strip the lines from the body; dedup
             * makes that idempotent. */
            extern size_t tur_hoist_top_includes_scan(const char *body, size_t len);
            if (f->as.cblock.p) {
                (void)tur_hoist_top_includes_scan(f->as.cblock.p, f->as.cblock.len);
            }
            /* inline-c-cname-module-prefix-plan: resolve __TUR_CNAME_<name>__
             * splices into captures so module-prefixed callees get their exact
             * C name (prefix + (export-as ...) alias). Unresolved names stay as
             * the mangle-only splice for the emit-time fallback. */
            InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
            Binding **cname_caps = NULL;
            uint8_t   cname_n_caps = 0;
            ic->code = elab_cblock_resolve_cnames(e, f->as.cblock, f->span,
                                                  &cname_caps, &cname_n_caps);
            ic->return_type = TYPE_NIL; /* Will be inferred from context or default to void */
            ic->captures = cname_caps;
            ic->n_captures = cname_n_caps;
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

/* LS2: file-scope holder for the active workspace-resolver context. Set
 * by main.c around its compile_to_c invocation; read here when Elab is
 * initialized. Stays NULL outside that scope (REPL, eval, etc.), so the
 * warning machinery is a no-op for those paths. Single-threaded usage. */
static const Ls2ResolverCtx *g_ls2_resolver_ctx;

void ls2_resolver_ctx_set(const Ls2ResolverCtx *ctx) {
    g_ls2_resolver_ctx = ctx;
}

const Ls2ResolverCtx *ls2_resolver_ctx_active(void) {
    return g_ls2_resolver_ctx;
}

/* used-attr-whole-program: file-scope holder for the active force-load list,
 * mirroring the LS2 context above.  Set by main.c around compile_to_c; read
 * after the main elaboration pass to retain unimported #[used] modules.
 * Single-threaded usage. */
static const UsedModulesCtx *g_used_modules_ctx;

void used_modules_ctx_set(const UsedModulesCtx *ctx) {
    g_used_modules_ctx = ctx;
}

const UsedModulesCtx *used_modules_ctx_active(void) {
    return g_used_modules_ctx;
}

/* Phase M: (load "path") expansion -- shared visited set + output accumulator
 * threaded through a depth-first, in-order recursive walk. */
typedef struct {
    Form         **out;        /* accumulated, fully-expanded form list */
    uint32_t       out_n, out_cap;
    int            rc;         /* -1 on any load error */
    /* Boundary tracking: the first `boundary_in` top-level INPUT forms are the
     * auto-loaded stdlib prefix. (load ...) expansion can splice in or elide
     * forms, so the stdlib region occupies a different number of OUTPUT slots
     * than its input count. `track_boundary` is set only on the outermost call
     * (whose loop index ranges over genuine top-level forms); when the loop
     * reaches input index `boundary_in`, `boundary_out` records how many output
     * forms the stdlib region produced -- the corrected stdlib_prefix. Without
     * this, the post-load stdlib boundary stays at the stale input count, and
     * the last auto-loaded defmodule's members fall past it (so the
     * stdlib macro-promotion sweep never reaches them).  See
     * docs/reported/autoload-defmodule-macro-not-promoted.md. */
    bool           track_boundary;
    uint32_t       boundary_in;
    uint32_t       boundary_out;
} LoadExpandCtx;

/* Normalize a (load "path") path into a stable dedup key. realpath() collapses
 * an absolute auto-load path (`/abs/.../stdlib/json.tur`) and a cwd-relative
 * user load (`stdlib/json.tur`) onto the same canonical string, so an explicit
 * re-load of an already-auto-loaded stdlib module is recognised as a duplicate
 * and skipped rather than re-spliced (which would collide with the auto-loaded
 * defns -- the "already defined by an auto-loaded stdlib module" hard error).
 * Falls back to the literal path when realpath() fails (e.g. an off-tree script
 * whose cwd-relative `stdlib/...` does not resolve until the stdlib fallback). */
static const Symbol *load_path_key(SymbolTable *st, const char *path) {
    char resolved[4096];
    if (realpath(path, resolved) != NULL)
        return intern_cstr(st, resolved);
    return intern_cstr(st, path);
}

/* Add a path key to the compilation-global load-visited set (idempotently). */
static void load_dedup_register(Elab *e, const Symbol *key) {
    for (uint32_t k = 0; k < e->n_load_expanded_paths; k++)
        if (e->load_expanded_paths[k] == key) return;
    if (e->n_load_expanded_paths >= e->cap_load_expanded_paths) {
        e->cap_load_expanded_paths = e->cap_load_expanded_paths
                                         ? e->cap_load_expanded_paths * 2 : 8;
        e->load_expanded_paths = (const Symbol **)realloc(
            (void *)e->load_expanded_paths,
            e->cap_load_expanded_paths * sizeof(const Symbol *));
        if (!e->load_expanded_paths) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    e->load_expanded_paths[e->n_load_expanded_paths++] = key;
}

static void load_expand_emit(LoadExpandCtx *lx, Arena *arena, Form *f) {
    if (lx->out_n >= lx->out_cap) {
        lx->out_cap = lx->out_cap ? lx->out_cap * 2 : 16;
        Form **n = (Form **)arena_alloc(arena, lx->out_cap * sizeof(Form *));
        for (uint32_t k = 0; k < lx->out_n; k++) n[k] = lx->out[k];
        lx->out = n;
    }
    lx->out[lx->out_n++] = f;
}

/* Expand every top-level (load "path") in `forms` in place, depth-first and
 * in source order: a file's transitive loads are fully expanded at the point
 * they appear, BEFORE the rest of that file's forms. A path is expanded at
 * most once (the visited set), and because the walk is depth-first a module
 * that self-loads a dependency always emits that dependency ahead of its own
 * dependent forms -- so e.g. range.tur's `(load "stdlib/typeclass.tur")`
 * lands its `defclass Show` before range.tur's `Show [Bound]` instance, even
 * when a sibling file later loads typeclass.tur explicitly. (The old
 * multi-pass fixpoint deferred transitive loads a pass behind sibling
 * explicit loads, letting the later one claim the path and relocate the
 * expansion; see docs/reported/load-not-idempotent-typeclass.md.) */
static void load_expand_forms(LoadExpandCtx *lx, Elab *e, Arena *arena,
                              SymbolTable *st, Form *const *forms, uint32_t nforms) {
    for (uint32_t i = 0; i < nforms; i++) {
        /* Record the corrected stdlib boundary the moment the outer walk
         * crosses the last stdlib input form (before emitting any user form). */
        if (lx->track_boundary && i == lx->boundary_in)
            lx->boundary_out = lx->out_n;
        Form *f = forms[i];

        /* Option A: descend into a (defmodule ...) body so a `(load "path")`
         * placed inside the module body splices the loaded file's forms into
         * the module's scope, exactly as a top-level load splices into the
         * compilation unit. Without this, a load nested in a defmodule body
         * survives the preprocessor and reaches elab_load, which errors. The
         * defmodule's head/name/export/import items are not load forms, so
         * they pass through this nested walk unchanged; only the `(load ...)`
         * body items expand in place. See
         * docs/archive/history/load-inside-defmodule-silently-loses-names.md. */
        if (f->tag == F_LIST && f->as.list.len >= 1 &&
            f->as.list.items[0]->tag == F_SYM &&
            f->as.list.items[0]->as.sym == e->sym_defmodule) {
            LoadExpandCtx sub = {0};
            sub.out_cap = f->as.list.len + 8;
            sub.out = (Form **)arena_alloc(arena, sub.out_cap * sizeof(Form *));
            /* Shares the compilation-global visited set on `e`, so a path
             * already spliced elsewhere is not re-spliced here. */
            load_expand_forms(&sub, e, arena, st,
                              f->as.list.items, f->as.list.len);
            if (sub.rc != 0) lx->rc = sub.rc;
            Form *expanded = form_list(arena, f->span, sub.out, sub.out_n);
            load_expand_emit(lx, arena, expanded);
            continue;
        }

        const Form *path_f = NULL;
        if (f->tag == F_LIST && f->as.list.len == 2 &&
            f->as.list.items[0]->tag == F_SYM &&
            f->as.list.items[0]->as.sym == e->sym_load &&
            f->as.list.items[1]->tag == F_STR) {
            path_f = f->as.list.items[1];
        }
        if (!path_f) { load_expand_emit(lx, arena, f); continue; }

        /* (load "path") -- read & parse */
        uint32_t plen = path_f->as.s.len;
        if (plen == 0 || plen >= 4096) {
            diag_emit(DIAG_ERROR, path_f->span,
                      "load: path must be non-empty and < 4096 chars");
            lx->rc = -1;
            continue;
        }
        char path_buf[4096];
        memcpy(path_buf, path_f->as.s.p, plen);
        path_buf[plen] = '\0';
        const Symbol *key = load_path_key(st, path_buf);
        /* The visited set is compilation-global (on the Elab) so a path the
         * entry already spliced is not re-spliced when an imported module loads
         * it too -- and vice versa. It is also seeded with the auto-loaded
         * stdlib files (see elaborate_program), so an explicit `(load
         * "stdlib/json.tur")` of a module that is already auto-loaded is a
         * no-op rather than a redefinition error. See load_expanded_paths in
         * elab_internal.h. */
        bool already = false;
        for (uint32_t k = 0; k < e->n_load_expanded_paths; k++) {
            if (e->load_expanded_paths[k] == key) { already = true; break; }
        }
        if (already) continue;  /* idempotent: a path is expanded at most once */
        /* Mark visited BEFORE recursing so a self/cyclic load is skipped. */
        load_dedup_register(e, key);

        char *src_raw = NULL;
        size_t src_len = 0;
        if (elab_read_file(path_buf, &src_raw, &src_len) != 0) {
            /* Off-tree fallback (one-off-script-print-and-annotation-ergonomics,
             * Finding 3): a freestanding `/tmp/foo.tur` that does
             * `(load "stdlib/math.tur")` cannot find the file cwd-relative when
             * run from outside the repo. `import` already falls back to the
             * resolved stdlib root (TUR_STDLIB_DIR, set absolute by main.c's
             * resolve_stdlib_root); mirror that here so the load-line the
             * "unknown function" hint suggests actually resolves off-tree.
             *
             * A "stdlib/<rest>" path is retried as "<stdlib_dir>/<rest>": the
             * resolved stdlib dir already ends in ".../stdlib", so the leading
             * "stdlib/" component is dropped to avoid ".../stdlib/stdlib/...".
             * When module_stdlib_dir is the legacy literal "stdlib" this
             * reproduces the original cwd-relative path (no regression). */
            bool recovered = false;
            const char *sdir = e->module_stdlib_dir;
            if (sdir && strncmp(path_buf, "stdlib/", 7) == 0) {
                char alt[4096];
                int an = snprintf(alt, sizeof(alt), "%s/%s", sdir, path_buf + 7);
                if (an > 0 && (size_t)an < sizeof(alt) &&
                    strcmp(alt, path_buf) != 0 &&
                    elab_read_file(alt, &src_raw, &src_len) == 0) {
                    memcpy(path_buf, alt, (size_t)an + 1);
                    plen = (uint32_t)an;
                    recovered = true;
                }
            }
            if (!recovered) {
                diag_emit(DIAG_ERROR, path_f->span, "load: cannot open '%s'", path_buf);
                lx->rc = -1;
                continue;
            }
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
        sfile->file_id = e->next_import_file_id++;
        sfile->reader_type = reader_type_from_extension(path_buf);
        if (sfile->reader_type == READER_UNKNOWN) sfile->reader_type = READER_TURMERIC;
        diag_register_file(sfile);
        /* Transitive-RM (T2): share the entry file's macro registry. */
        uint32_t lf_n = 0;
        bool had_error_before_load = diag_had_error();
        Form **lf = read_all_with_registry(arena, st, sfile, e->user_macros, &lf_n);
        if (!lf) {
            if (!had_error_before_load && diag_had_error()) {
                diag_emit(DIAG_NOTE, path_f->span, "while loading '%s'", path_buf);
            }
            lx->rc = -1;
            continue;
        }
        /* Depth-first: expand this file's own loads in place before continuing. */
        load_expand_forms(lx, e, arena, st, lf, lf_n);
    }
}

/* load-not-expanded-in-imported-or-project-modules: expand the top-level
 * (load "path") forms of an *imported* module's form list the same way
 * elaborate_program does for the entry unit. The entry preprocessor only ran
 * over the entry's own forms, so without this an imported file's top-level
 * `(load ...)` survived to elaboration and errored ("load is only valid at the
 * top level"). The visited set lives on the Elab and is shared with the entry
 * expansion, so a path loaded by both is spliced exactly once.
 *
 * Writes the fully-expanded form list to *out_forms / *out_n (arena-allocated)
 * and returns 0 on success, -1 if any load failed. */
int elab_expand_module_loads(Elab *e, Arena *arena, SymbolTable *st,
                             Form *const *forms, uint32_t nforms,
                             Form ***out_forms, uint32_t *out_n) {
    LoadExpandCtx lx = {0};
    lx.out_cap = nforms + 16;
    lx.out = (Form **)arena_alloc(arena, lx.out_cap * sizeof(Form *));
    load_expand_forms(&lx, e, arena, st, forms, nforms);
    *out_forms = lx.out;
    *out_n = lx.out_n;
    return lx.rc;
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
                        uint32_t *out_n_file_scope_defs,
                        struct ReaderMacroRegistry *user_macros) {
    Elab e;
    elab_init_state(&e, arena, st);
    e.user_macros = user_macros;
    e.module_base_dir = module_base_dir ? module_base_dir : ".";
    /* stdlib fallback: TUR_STDLIB_DIR env var, else "stdlib" */
    {
        const char *sdir = getenv("TUR_STDLIB_DIR");
        e.module_stdlib_dir = (sdir && *sdir) ? sdir : "stdlib";
    }
    e.module_include_dirs   = include_dirs;
    e.n_module_include_dirs = n_include_dirs;
    /* LS2: pull workspace-resolution context published by main.c around
     * the compile_to_c call (NULL outside that path -- the warning code
     * then silently no-ops). */
    {
        const Ls2ResolverCtx *ctx = ls2_resolver_ctx_active();
        if (ctx && ctx->n_inc == n_include_dirs) {
            e.module_include_workspace_producer = ctx->producer_per_inc;
            e.module_include_warned             = ctx->warned_per_inc;
            e.module_consumer_declared_spices   = ctx->declared_spices;
            e.n_module_consumer_declared_spices = ctx->n_declared_spices;
        }
    }
    e.separate_compilation  = separate_compilation;
    e.sandboxed             = sandboxed;
    builtins_init(st);

    int rc = 0;

    /* Seed the load-visited set with the auto-loaded stdlib files. main.c
     * pre-splices the stdlib forms[0..stdlib_prefix) directly (not as `(load
     * ...)` forms), so they never pass through the dedup below. Registering
     * their canonical paths here makes a later explicit `(load
     * "stdlib/json.tur")` of an already-auto-loaded module (json/schema are
     * unconditionally auto-loaded since the -X reader flags became no-ops) a
     * no-op instead of a "already defined by an auto-loaded stdlib module"
     * collision. Distinct file_ids in the stdlib region map 1:1 to the
     * auto-loaded source files. */
    {
        uint16_t seen_file = (uint16_t)-1;
        for (uint32_t i = 0; i < stdlib_prefix && i < nforms; i++) {
            uint16_t fid = forms[i]->span.file_id;
            if (fid == seen_file) continue;  /* runs are contiguous per file */
            seen_file = fid;
            const SourceFile *sf = diag_source_file(fid);
            if (sf && sf->path)
                load_dedup_register(&e, load_path_key(st, sf->path));
        }
    }

    /* Phase M: (load "path") preprocessing.
     * Expand all top-level (load "path") forms in place, depth-first and in
     * source order, parsing each referenced file and splicing its (already
     * expanded) forms into the form list. A shared visited set keeps each path
     * to a single expansion and breaks cycles. Loaded forms then participate
     * in the normal two-pass elaboration. */
    {
        LoadExpandCtx lx = {0};
        lx.out_cap = nforms + 16;
        lx.out = (Form **)arena_alloc(arena, lx.out_cap * sizeof(Form *));
        lx.track_boundary = (stdlib_prefix > 0);
        lx.boundary_in    = stdlib_prefix;
        lx.boundary_out   = stdlib_prefix; /* default if the loop never crosses it */
        load_expand_forms(&lx, &e, arena, st, forms, nforms);
        if (lx.rc != 0) rc = lx.rc;
        /* If the boundary sat at the very end (no user forms), the loop never
         * reached an index == boundary_in, so the post-expansion stdlib region
         * is everything emitted. */
        if (lx.track_boundary && stdlib_prefix >= nforms)
            lx.boundary_out = lx.out_n;
        /* Replace forms/nforms with the expanded list for the rest of
         * elaborate_program.  Cast away const since we're in our own copy. */
        forms = (Form *const *)lx.out;
        nforms = lx.out_n;
        /* Re-anchor the stdlib boundary onto the expanded form stream so the
         * stdlib promotion sweep and in_stdlib_load window line up with where
         * the auto-loaded forms actually landed. */
        if (lx.track_boundary) stdlib_prefix = lx.boundary_out;
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
        /* range-gadt-typeclass-migration-plan A1: GADTs are registered
         * unconditionally. */
        Form *name_f = f->as.list.items[1];
        if (!name_f || name_f->tag != F_SYM) continue;
        const Symbol *type_name = name_f->as.sym;
        /* Skip if already in scope (e.g. stdlib defines a type with this name) */
        if (scope_lookup(&e.global, type_name)) continue;
        /* Pre-allocate a stub def and register a forward binding */
        elab_add_forward_type(&e, type_name);
        /* DS5: memset each stub before populating named fields so newly
         * added bool / scalar fields default to 0 -- arena_alloc returns
         * uninitialised memory and UBSan tripped on reads of
         * `is_opaque` / other bools that subsequent passes (e.g. emit
         * pass 0 in emit_module.c) consult. */
        if (is_defopaque) {
            /* structdef-retirement slice 5: an opaque newtype is now an opaque
             * AdtDef (n_ctors == 0), not a StructDef, so StructDef can be retired.
             * The stub mirrors the old struct stub: a copyable, opaque, named
             * int64 carrier whose phantom arity makes `(Name A)` annotations
             * kind-check before the full defopaque elaboration fills it in. */
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            memset(stub, 0, sizeof(*stub));
            stub->name = type_name->name;
            stub->is_copy = true;
            stub->is_opaque = true;
            stub->origin_file_id = name_f->span.file_id;
            /* Parameterized opaque: an optional [A ...] vector after the name
             * makes this a type constructor.  Record the arity on the stub so
             * `(Name A)` annotations resolved before the full defopaque
             * elaboration kind-check against the right arrow kind. */
            uint8_t opaque_arity = 0;
            if (f->as.list.len >= 3 && f->as.list.items[2]->tag == F_VEC) {
                opaque_arity = (uint8_t)f->as.list.items[2]->as.list.len;
            }
            stub->n_type_params = opaque_arity;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            t.hkt_kind = kind_for_arity(opaque_arity);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else if (is_defstruct) {
            /* CONV-S1 (defstruct-as-defadt): a defstruct lowers to a
             * single-variant record defadt, so pre-register an ADT stub (not a
             * struct stub) -- the later elab_defstruct rewrite to elab_defdata
             * fills this stub exactly as a real defdata would.
             * structdef-retirement DS-C: defstruct_lowers_to_adt is always true
             * now (every field shape lowers or is rejected at the ADT field
             * parser), so the former `else if (is_defstruct)` StructDef-stub
             * branch was unreachable and is removed. */
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            memset(stub, 0, sizeof(*stub));
            stub->name = type_name->name;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else if (is_defgadt) {
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            memset(stub, 0, sizeof(*stub));
            stub->name = type_name->name;
            stub->is_gadt = true;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else {
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            memset(stub, 0, sizeof(*stub));
            stub->name = type_name->name;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        }
    }

    /* Phase 2: Two-pass elaboration for mutual recursion support.
     * Pass 1: Collect all top-level defn declarations and add them to scope.
     * This allows mutually recursive functions to see each other. */
    e.in_stdlib_load = (stdlib_prefix > 0);
    for (uint32_t i = 0; i < nforms; i++) {
        if (i == stdlib_prefix) e.in_stdlib_load = false;
        Form *f = forms[i];
        if (f->tag == F_LIST && f->as.list.len > 0) {
            Form *head = f->as.list.items[0];
            if (head->tag == F_SYM) {
                if (head->as.sym == e.sym_defn) {
                    /* Parse defn declaration without body */
                    if (f->as.list.len >= 3) {
                        /* Phase R5: skip optional #[no-unwind] / #[used] bare
                         * attribute symbols (either order) before the name. */
                        uint32_t name_idx = 1;
                        while ((uint32_t)f->as.list.len > name_idx &&
                               f->as.list.items[name_idx]->tag == F_SYM &&
                               (f->as.list.items[name_idx]->as.sym == e.sym_no_unwind_attr ||
                                f->as.list.items[name_idx]->as.sym == e.sym_used_attr)) {
                            name_idx++;
                        }
                        /* Phase M6: skip optional (export-as "c_name") attribute */
                        if ((uint32_t)f->as.list.len > name_idx &&
                            f->as.list.items[name_idx]->tag == F_LIST &&
                            f->as.list.items[name_idx]->as.list.len == 2 &&
                            f->as.list.items[name_idx]->as.list.items[0]->tag == F_SYM &&
                            f->as.list.items[name_idx]->as.list.items[0]->as.sym == e.sym_export_as_attr) {
                            name_idx += 1; /* skip (export-as "c_name") */
                        }
                        /* F4: skip optional ^deprecated [message] attribute */
                        if ((uint32_t)f->as.list.len > name_idx &&
                            f->as.list.items[name_idx]->tag == F_SYM &&
                            f->as.list.items[name_idx]->as.sym == e.sym_caret_deprecated) {
                            name_idx += 1;
                            if ((uint32_t)f->as.list.len > name_idx &&
                                f->as.list.items[name_idx]->tag == F_STR) {
                                name_idx += 1;
                            }
                        }
                        if ((uint32_t)f->as.list.len <= name_idx) goto next_form;
                        Form *name_f = f->as.list.items[name_idx];
                        if (name_f->tag == F_SYM) {
                            /* Parse return type annotation if present */
                            TypeKind return_kind = TY_INT; /* default */
                            uint32_t params_idx_local = name_idx + 1; /* params usually here */
                            /* poly-defn-recursive-return-type-inference: a defn with
                             * explicit type parameters spells as
                             *   (defn name [TypeVars] [params] :ret body)        -- 2-vec
                             *   (defn name [TypeVars] [Constraints] [params] :ret body) -- 3-vec
                             * so the params vector is at name_idx+2 (or +3), not +1.
                             * Without this skip, ret_idx points at the params vec, the
                             * keyword/sym/type-ann probe below misses, and the forward
                             * decl falls back to TY_INT -- breaking recursive self-calls
                             * inside a poly-defn body (e.g. `: bool` typed as int).
                             * Mirrors the F_VEC detection in elab_fns.c elab_defn. */
                            if (f->as.list.len > params_idx_local + 1 &&
                                f->as.list.items[params_idx_local]->tag == F_VEC &&
                                f->as.list.items[params_idx_local + 1]->tag == F_VEC) {
                                /* type-param vec present; bump past it */
                                params_idx_local++;
                                /* optional constraint vec between TypeVars and params */
                                if (f->as.list.len > params_idx_local + 1 &&
                                    f->as.list.items[params_idx_local]->tag == F_VEC &&
                                    f->as.list.items[params_idx_local + 1]->tag == F_VEC) {
                                    params_idx_local++;
                                }
                            }
                            uint32_t ret_idx = params_idx_local + 1; /* :ret follows params */
                            /* Skip optional #{Unsafe} / effect-row annotation (F_MAP) */
                            if (f->as.list.len > ret_idx && f->as.list.items[ret_idx]->tag == F_MAP) {
                                ret_idx++;
                            }
                            if (f->as.list.len > ret_idx) {
                                Form *ret_f = f->as.list.items[ret_idx];
                                /* Accept spaced `: T` (F_TYPE_ANN of a single
                                 * symbol/keyword) by treating the inner as a
                                 * keyword.  Compound `: (-> a b)` still routes
                                 * through the F_TYPE_ANN branch below. */
                                if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1) {
                                    Form *inner = ret_f->as.list.items[0];
                                    if (inner->tag == F_SYM || inner->tag == F_KEYWORD) {
                                        ret_f = inner;
                                    } else if (inner->tag == F_NIL) {
                                        /* `: nil` -- the bare `nil` literal in a type
                                         * position is parsed as F_NIL by the reader;
                                         * it means the nil/void return type. */
                                        return_kind = TY_NIL;
                                        ret_f = NULL;
                                    }
                                }
                                if (ret_f && (ret_f->tag == F_KEYWORD || ret_f->tag == F_SYM)) {
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
                                } else if (ret_f && ret_f->tag == F_TYPE_ANN && ret_f->as.list.len > 0) {
                                    /* Compound return type: peek at the head symbol to
                                     * recognize Session[P] returns for pass-1 forward decls. */
                                    Form *head_f = ret_f->as.list.items[0];
                                    if (head_f->tag == F_SYM &&
                                            strcmp(head_f->as.sym->name, "Session") == 0) {
                                        return_kind = TY_SESSION;
                                    }
                                }
                            }
                            /* Count actual arity + scalar arg kinds from the
                             * params vector.  fwd_decl_scan_params skips
                             * `^`-prefixed markers (^fat/^mut/...) so the
                             * forward-declared arity is not over-stated (see
                             * docs/reported/pap-defmodule-fat-fn-too-many-args.md). */
                            TypeKind arg_kinds[MAX_FN_ARITY];
                            uint32_t param_arity = (name_idx + 1 < (uint32_t)f->as.list.len)
                                ? fwd_decl_scan_params(f->as.list.items[name_idx + 1], arg_kinds)
                                : 0;
                            Type fn_type = type_fn(arg_kinds, param_arity, return_kind);
                            /* MF3: if the name is already in global scope (e.g. an
                             * auto-loaded stdlib defn), do NOT pre-register a
                             * duplicate forward decl. Pass 2's elab_defn will then
                             * see the original binding (with is_from_stdlib set
                             * correctly) and either reuse it as a forward decl or
                             * emit the shadow diagnostic. */
                            if (!scope_lookup(&e.global, name_f->as.sym)) {
                                Binding *b = binding_new(&e, name_f->as.sym, fn_type, false, true, f->span);
                                scope_add(&e.global, b);
                            }
                        }
                    }
                    next_form:;
                }
            }
        }
    }

    /* Phase M0+: Validate defmodule position per source file.
     *
     * "defmodule must be the first form in the file" -- where "the file"
     * is the source file that contributes the form, not the whole
     * compilation unit.  Each (load ...)-spliced file gets its own
     * scope for the check, so a defmodule-wrapped spice file loaded
     * after a flat stdlib helper is accepted.
     *
     * The check uses forms[i]->span.file_id as the file-of-origin key;
     * each loaded SourceFile is assigned a unique id at parse time
     * (see the (load ...) preprocessing loop above, line ~668).
     *
     * Continues past the first defmodule so misplaced defmodules in
     * later loaded files also surface. */
    {
        uint32_t cur_file       = (uint32_t)-1;
        uint32_t file_start_idx = stdlib_prefix;
        for (uint32_t i = stdlib_prefix; i < nforms; i++) {
            Form *f = forms[i];
            if (f->span.file_id != cur_file) {
                cur_file       = f->span.file_id;
                file_start_idx = i;
            }
            if (f->tag != F_LIST || f->as.list.len == 0) continue;
            Form *head = f->as.list.items[0];
            if (head->tag != F_SYM || head->as.sym != e.sym_defmodule) continue;
            if (i == file_start_idx) continue;
            /* If the file-start form is itself a defmodule, defer to the
             * "only one defmodule is allowed per file" check in
             * elab_module.c -- that diagnostic is more specific. */
            Form *first = forms[file_start_idx];
            if (first->tag == F_LIST && first->as.list.len > 0) {
                Form *fh = first->as.list.items[0];
                if (fh->tag == F_SYM && fh->as.sym == e.sym_defmodule) continue;
            }
            diag_emit(DIAG_ERROR, head->span,
                      "defmodule must be the first form in the file");
            diag_emit(DIAG_NOTE, forms[file_start_idx]->span,
                      "this form comes before defmodule in the same file; "
                      "move it inside the defmodule body or below it");
            rc = -1;
        }
    }
    if (rc != 0) {
        scope_free(&e.global);
        free(e.struct_defs);
        free(e.adt_defs);
        free(e.forward_type_syms);
        free(e.handled_effect_names);
        free(e.macros);
        free(e.macro_expansion_stack);
        free((void *)e.load_expanded_paths);
        return NULL;
    }

    /* Pass 2: Elaborate all forms */
    e.in_stdlib_load = (stdlib_prefix > 0);
    for (uint32_t i = 0; i < nforms; i++) {
        if (i == stdlib_prefix) e.in_stdlib_load = false;
        items[i] = elab_form(&e, forms[i]);
        if (!items[i]) { rc = -1; /* keep going to surface more diagnostics */ }

        /* Phase M7+: Each (load ...)-spliced file is conceptually its own
         * file, so reset has_defmodule at every file boundary -- not just
         * after stdlib defmodules.  Without this, a user program that
         * loads two defmodule-wrapped files in sequence would trip the
         * "one defmodule per file" check on the second file.
         *
         * The original M7 reset triggered on the defmodule form itself,
         * which works for stdlib (every auto-loaded file has a
         * defmodule).  Generalising to "any file boundary" subsumes that
         * case and additionally handles user-loaded files. */
        if (i + 1 < nforms &&
            forms[i + 1]->span.file_id != forms[i]->span.file_id) {
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
                    gb->is_from_stdlib = true;
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

    /* used-attr-whole-program: force-load any #[used]-bearing modules that the
     * entry reaches only via a raw mangled C symbol (no `(import)`), so their
     * defns are emitted into this single TU and the extern resolves at link
     * time.  Runs after the main pass (an already-imported module is deduped)
     * and before the file-scope prepend below (so the loaded module's
     * EX_DEFMODULE, registered via elab_register_file_def during the load, is
     * picked up).  Inert under separate compilation and when no list is set. */
    if (rc == 0) {
        const UsedModulesCtx *umc = used_modules_ctx_active();
        if (umc && umc->modules && !separate_compilation) {
            for (int i = 0; i < umc->n; i++)
                elab_force_load_module(&e, umc->modules[i]);
            if (diag_had_error()) rc = -1;
        }
    }

    /* bare-fat-result-monomorphization (Phase B): surface the deferred
     * diagnostic for any lazy bare-^fat binding that no call site specialized
     * (e.g. a float-only combinator that is defined but never called).  Must
     * run after the whole program is elaborated so all call sites have had a
     * chance to specialize.  Done before the file-scope prepend so its emitted
     * specializations (registered during the main loop) are already counted. */
    elab_sweep_bare_fat_lazy(&e);
    free(e.bare_fat_specs);
    free(e.bare_fat_lazy_bindings);

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

    /* method-vs-defn clash warning (TUR-W0039).
     *
     * A typeclass method and a free top-level `defn` share the same value
     * namespace.  The two now coexist (fix (1) of
     * docs/reported/typeclass-methods-share-value-namespace-with-defns.md): a
     * bare `(name x ...)` dispatches to the matching instance when the
     * receiver's static type selects one, and falls back to the free defn
     * otherwise (see the `prefer_method_dispatch` gate in elab_call.c).  The
     * clash is no longer a silent footgun, but the resolution rule -- a method
     * can win over a same-named defn for some receiver types -- is still worth
     * surfacing so the author is not surprised.
     *
     * Because *overriding a stdlib class method* with a same-named user defn is
     * a documented, intentional pattern (and stdlib methods keep "defn wins"),
     * the warning fires only when the colliding class is user-defined
     * (`!tc->from_stdlib`) and the binding is genuine user code
     * (`!is_from_stdlib`).  It is a warning, not an error.  The dotted
     * `(.method ...)` form always dispatches the method regardless. */
    for (TypeClass *tc = e.typeclass_env.typeclasses; tc != NULL; tc = tc->next) {
        if (tc->from_stdlib) continue;
        for (uint8_t mi = 0; mi < tc->n_methods; mi++) {
            const Symbol *mn = tc->methods[mi].name;
            if (!mn) continue;
            Binding *b = scope_lookup(&e.global, mn);
            if (!b || b->is_from_stdlib) continue;
            diag_emit_with_code(DIAG_WARNING, b->span, TUR_W0039_METHOD_DEFN_CLASH,
                "free defn '%s' shares its name with the method '%s' of typeclass "
                "'%s'; a bare (%s ...) dispatches to the method when the receiver "
                "type has an instance, and falls back to this defn otherwise -- "
                "rename one, or use the dotted form (.%s ...) to force dispatch",
                mn->name, mn->name, tc->name->name, mn->name, mn->name);
        }
    }

    scope_free(&e.global);
    free(e.struct_defs);
    free(e.adt_defs);
    free(e.forward_type_syms);
    free(e.handled_effect_names);
    free(e.macros);
    free(e.macro_expansion_stack);
    free(e.loaded_modules); /* Phase M2 */
    free(e.dynvar_entries);
    free(e.active_dynvar_bindings);
    free((void *)e.load_expanded_paths);
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
