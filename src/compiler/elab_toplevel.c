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
    const StructDef *target_struct = NULL;
    Type result_type;
    if (target_kind == TY_UNKNOWN) {
        /* TY2.2: a struct/ADT name is a valid cast target. */
        Type *named = elab_lookup_type_by_name(e, type_form->as.sym);
        if (!named) {
            diag_emit(DIAG_ERROR, type_form->span,
                      "unknown type '%s' in 'cast'", type_form->as.sym->name);
            return NULL;
        }
        target_kind = named->kind;
        result_type = *named;
        if (named->kind == TY_STRUCT) target_struct = named->as.struct_.def;
    } else {
        result_type = type_simple(target_kind, CK_COPY);
    }
    Expr *out = expr_new(e->arena, EX_ANY_CAST, result_type, call->span);
    out->as.any_cast_.value = val;
    out->as.any_cast_.target_kind = target_kind;
    out->as.any_cast_.target_struct = target_struct;
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
        test_kind = named->kind;
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
    /* SYM3 (runtime-symbols-plan): under -Xsymbols a keyword key is a
     * first-class :Sym value -- pass the F_KEYWORD through so the map is keyed
     * by Sym (pointer-identity, via Hash[Sym] / MapKey[Sym]) instead of
     * decaying to a content-hashed cstr.  Without the flag the legacy
     * hamt/hash-str lowering below is unchanged. */
    if (key->tag == F_KEYWORD && g_symbols_enabled) return key;
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
            /* SYM0 (runtime-symbols-plan): under -Xsymbols, a keyword in
             * expression position is a first-class :Sym literal whose runtime
             * value is a pointer-identity-equal interned symbol.  Without the
             * flag it remains a hard error (its only legal uses are syntactic:
             * type annotations, :refer/:as, struct-field selectors, ADT tags --
             * all consumed by earlier passes before reaching elab_form). */
            if (g_symbols_enabled) {
                Expr *out = expr_new(e->arena, EX_SYM_LIT, TYPE_SYM, f->span);
                out->as.sym_lit_.sym = f->as.sym;
                return out;
            }
            diag_emit(DIAG_ERROR, f->span,
                      "keyword in expression position requires -Xsymbols");
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
            /* DL1: in expression position (i.e. not consumed structurally by a
             * binding form), a [...] vector lowers to (vec-of ...).  Binding
             * forms (defn/fn/let/loop/...) grab their F_VEC slot before it ever
             * reaches elab_form, so reaching here means expression position. */
            if (g_data_literals_enabled) {
                Form *call = dl_build_call(e, f->span, "vec-of",
                                           f->as.list.items, f->as.list.len);
                return elab_form(e, call);
            }
            diag_emit(DIAG_ERROR, f->span,
                      "phase 1: vector literals are only allowed in let bindings");
            return NULL;
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
                /* Transitive-RM (T2): use the shared `user_macros`
                 * registry so the loaded file sees the entry file's
                 * macros. `(load ...)` and `(import ...)` populate the
                 * same registry -- see docs/reader-macros-plan.md
                 * ("Loading semantics") for the user-facing contract. */
                uint32_t lf_n = 0;
                bool had_error_before_load = diag_had_error();
                Form **lf = read_all_with_registry(arena, st, sfile,
                                                   e.user_macros, &lf_n);
                if (!lf) {
                    /* Same import-chain note as elab_module.c's T1
                     * handling (decision #4). */
                    if (!had_error_before_load && diag_had_error()) {
                        diag_emit(DIAG_NOTE, path_f->span,
                                  "while loading '%s'", path_buf);
                    }
                    rc = -1;
                    continue;
                }
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
        /* range-gadt-typeclass-migration-plan A1: GADTs are registered
         * unconditionally now that g_gadt_enabled defaults true. */
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
            StructDef *stub = (StructDef *)arena_alloc(arena, sizeof(StructDef));
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
            elab_register_struct_def(&e, stub);
            Type t = type_struct(stub);
            t.hkt_kind = kind_for_arity(opaque_arity);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else if (is_defstruct) {
            StructDef *stub = (StructDef *)arena_alloc(arena, sizeof(StructDef));
            memset(stub, 0, sizeof(*stub));
            stub->name = type_name->name;
            stub->origin_file_id = name_f->span.file_id;
            elab_register_struct_def(&e, stub);
            Type t = type_struct(stub);
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
                            uint32_t ret_idx = name_idx + 2; /* name params :ret */
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
                                if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1 &&
                                    (ret_f->as.list.items[0]->tag == F_SYM ||
                                     ret_f->as.list.items[0]->tag == F_KEYWORD)) {
                                    ret_f = ret_f->as.list.items[0];
                                }
                                if (ret_f->tag == F_KEYWORD || ret_f->tag == F_SYM) {
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
                            /* Count actual arity from params vector (non-keyword items are params) */
                            uint32_t param_arity = 0;
                            if (name_idx + 1 < (uint32_t)f->as.list.len) {
                                Form *params_f = f->as.list.items[name_idx + 1];
                                if (params_f->tag == F_VEC) {
                                    for (uint32_t pi = 0; pi < params_f->as.list.len; pi++) {
                                        Form *p = params_f->as.list.items[pi];
                                        if (p->tag != F_KEYWORD && p->tag != F_TYPE_ANN)
                                            param_arity++;
                                    }
                                }
                            }
                            if (param_arity > MAX_FN_ARITY) param_arity = MAX_FN_ARITY;
                            TypeKind arg_kinds[MAX_FN_ARITY];
                            for (uint32_t ai = 0; ai < param_arity; ai++) arg_kinds[ai] = TY_INT;
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
    free(e.dynvar_entries);
    free(e.active_dynvar_bindings);
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
