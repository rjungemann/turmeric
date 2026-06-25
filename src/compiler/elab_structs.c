/* elab_structs.c -- struct/ADT/GADT definitions, pattern matching, and borrow traits. */
#include "elab_internal.h"
#include "experiments.h"  /* CONV-S1: defstruct-as-defadt experiment gate */

/* ---- file-local helper forward declarations ---- */
static void parse_struct_field_type(const char *tname, uint32_t tlen,
    TypeKind *out_kind, TypeKind *out_inner);
static bool typekind_is_copy_for_struct(TypeKind k);
/* Exposed (declared in elab_internal.h) for CTOR-V0 struct-name call routing. */
static bool elab_is_forward_type(Elab *e, const Symbol *sym);
static Type gadt_resolve_type_from_form(Elab *e, const AdtDef *gadt, const Form *f,
    const SkolemEnv *senv);
static void gadt_build_skolem_env(Elab *e, SkolemEnv *out, const AdtDef *def,
    const CtorDef *ctor);
static TypeKind gadt_field_typekind_from_form(const Form *f);
static Type *adt_field_type_from_form(Arena *arena, const Form *ft_form,
    const char **type_params, uint8_t n_type_params);
static void infer_type_param_kinds(AdtDef *def);
static void infer_struct_type_param_kinds(StructDef *def, Kind *field_type_param_kinds);

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
Binding *scope_lookup_type_def(Scope *s, const Symbol *name) {
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

/* CONV-S1 (slice 5): the ADT-record analogue of lookup_rc_inner_struct_def.
 * When a record-variant field is annotated `rc<Name>` and `Name` resolves to an
 * in-scope struct or single-variant record ADT, build the inner-carrying rc Type
 * (`type_rc_struct` / `type_rc_adt`) so field access through the rc receiver can
 * auto-deref to the named field -- exactly the surface a `defstruct` rc<Struct>
 * field already exposes (DS3 / slice 2), now reached by the lowered struct path.
 * Returns NULL (the field stays a bare rc carrier) when `tname` is not an
 * `rc<...>` over a known aggregate, so a scalar inner (`rc<int>`) or an unknown
 * name is unaffected. */
static Type *adt_rc_inner_full_type(Elab *e, const char *tname, uint32_t tlen) {
    if (tlen <= 4 || memcmp(tname, "rc<", 3) != 0 || tname[tlen - 1] != '>') {
        return NULL;
    }
    const char *inner_name = tname + 3;
    uint32_t inner_len = tlen - 4;  /* strip "rc<" and ">" */
    const Symbol *sym = symtab_intern(e->st, strslice(inner_name, inner_len));
    Binding *tb = scope_lookup_type_def(e->scope, sym);
    if (!tb) return NULL;
    Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
    if (tb->type.kind == TY_STRUCT) {
        *t = type_rc_struct(tb->type.as.struct_.def);
        return t;
    }
    if (tb->type.kind == TY_ADT) {
        *t = type_rc_adt(tb->type.as.adt_.def);
        return t;
    }
    return NULL;
}


static bool struct_type_param_index(const StructDef *def, const char *name, uint8_t *out_idx) {
    if (!def || !name) return false;
    for (uint8_t i = 0; i < def->n_type_params; i++) {
        if (def->type_params[i] && strcmp(def->type_params[i], name) == 0) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

static bool struct_type_has_named_tyvar(const StructDef *def, const Type *t) {
    if (!def || !t) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name && struct_type_param_index(def, t->as.tyvar_.name, NULL);
        case TY_APP:
            return struct_type_has_named_tyvar(def, t->as.app.fn)
                || struct_type_has_named_tyvar(def, t->as.app.arg);
        case TY_UNION:
            for (uint8_t i = 0; i < t->as.union_.n_members; i++) {
                if (struct_type_has_named_tyvar(def, t->as.union_.members[i])) return true;
            }
            return false;
        case TY_INTERSECTION:
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++) {
                if (struct_type_has_named_tyvar(def, t->as.intersection_.members[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

/* defstruct-byvalue-struct-field-stored-as-int-carrier: decide the STORAGE
 * kind for a defstruct field whose type is a bare (nullary) user struct/ADT,
 * and record its nominal `full_type` for read-back.  Three cases:
 *
 *   - opaque newtype  -> int64 carrier (TY_INT); full_type kept so `(.f x)`
 *     reads back as the declared opaque, not `:int`.  type_c_name lowers the
 *     opaque to int64_t, so storage stays carrier-consistent.
 *   - ADT             -> int64 carrier (TY_INT); full_type kept (ADT carriers
 *     are int64-consistent on both the store and the access path).
 *   - by-value struct -> stored INLINE by value (TY_STRUCT) with full_type, so
 *     make-struct initializes the inline aggregate and `(.f x)` reads the
 *     struct back -- both representations agree.  A direct self-reference
 *     (`(defstruct Node [next : Node])`) would be an infinite-size aggregate,
 *     so it stays on the int64 carrier (a self-link is necessarily a pointer).
 *
 * `owner` is the struct currently being defined (for the self-reference check);
 * it may be NULL/partially-filled for a forward stub, which is fine -- the
 * pointer-identity compare just won't match and we keep the by-value path.
 */
static TypeKind struct_field_user_type_storage(const Binding *tb,
                                               const StructDef *owner,
                                               Type **out_full_type,
                                               Arena *arena) {
    bool is_byvalue_struct =
        tb->type.kind == TY_STRUCT &&
        tb->type.as.struct_.def &&
        !tb->type.as.struct_.def->is_opaque &&
        tb->type.as.struct_.def != owner;  /* not a direct self-link */
    if (is_byvalue_struct) {
        Type *t = (Type *)arena_alloc(arena, sizeof(Type));
        *t = tb->type;
        *out_full_type = t;
        return TY_STRUCT;
    }
    /* Opaque newtype: int64 carrier storage, but keep the nominal type so
     * `(.f x)` reads back as the declared opaque rather than `:int`.  ADTs and
     * self-referential struct links stay on the bare int64 carrier with no
     * recorded full_type (their existing carrier-consistent behavior). */
    if (tb->type.kind == TY_STRUCT &&
        tb->type.as.struct_.def &&
        tb->type.as.struct_.def->is_opaque) {
        Type *t = (Type *)arena_alloc(arena, sizeof(Type));
        *t = tb->type;
        *out_full_type = t;
    }
    return TY_INT;
}

/* defstruct-field-byvalue-parametric-struct-layout: is `t` a concrete,
 * by-value (non-`:heap`) parametric struct application -- e.g. `(Option cstr)`?
 * Such a value is an embedded aggregate (`Option__cstr`), NOT the int64 carrier,
 * so a field of this type must be laid out inline by value just like a bare
 * nullary by-value struct field.  Heap containers (`(Cons int)`, `(Vec int)`)
 * are already int64-carried typed pointers and stay on the carrier; opaque
 * newtypes and transparent-int newtypes also stay int64.  A non-concrete app
 * (a field `(Option A)` over the owner's own type parameter) has no fixed size
 * and likewise stays on the carrier. */
static bool struct_field_app_is_byvalue_struct(const Type *t) {
    if (!t || t->kind != TY_APP) return false;
    if (!type_has_concrete_codegen_layout(t)) return false;
    if (type_is_heap_struct(*t)) return false;
    if (type_is_transparent_int_newtype(*t)) return false;
    StructDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_struct_app(t, &def, args, &n_args)) return false;
    return def && !def->is_opaque;
}

static void struct_field_storage_from_type(const Type *t, TypeKind *out_kind, TypeKind *out_inner) {
    *out_kind = TY_UNKNOWN;
    *out_inner = TY_UNKNOWN;
    if (!t) return;
    switch (t->kind) {
        case TY_APP:
            /* defstruct-field-byvalue-parametric-struct-layout: store a concrete
             * by-value parametric struct (`(Option cstr)`) inline as the embedded
             * aggregate, matching the value make-struct provides and the `.field`
             * accessor expects.  Carrier-shaped apps fall through to int64. */
            if (struct_field_app_is_byvalue_struct(t)) {
                *out_kind = TY_STRUCT;
                return;
            }
            *out_kind = TY_INT;
            return;
        case TY_TYVAR:
        case TY_EXISTS:
        case TY_FORALL:
        case TY_STRUCT:
        case TY_ADT:
            *out_kind = TY_INT;
            return;
        case TY_REF:
        case TY_LREF:
            *out_kind = t->kind;
            *out_inner = t->as.ref.inner;
            return;
        case TY_RC:
        case TY_WEAK:
            *out_kind = t->kind;
            *out_inner = t->as.rc.inner;
            return;
        default:
            *out_kind = t->kind;
            return;
    }
}

static Type *struct_field_type_from_form(Elab *e, const Form *form,
                                         const Symbol **type_params,
                                         Kind *type_param_kinds,
                                         uint8_t n_type_params) {
    if (!form) return NULL;
    if (form->tag == F_TYPE_ANN && form->as.list.len > 0) {
        return struct_field_type_from_form(e, form->as.list.items[0],
                                           type_params, type_param_kinds, n_type_params);
    }
    if (form->tag == F_SYM) {
        for (uint8_t i = 0; i < n_type_params; i++) {
            if (type_params[i] == form->as.sym) {
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_tyvar_named(form->as.sym->name);
                t->hkt_kind = type_param_kinds ? type_param_kinds[i] : KIND_STAR;
                return t;
            }
        }
        return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
    }
    if (form->tag == F_LIST && form->as.list.len >= 1 &&
        form->as.list.items[0]->tag == F_SYM) {
        const Symbol *head = form->as.list.items[0]->as.sym;
        if (head == e->sym_forall || head == e->sym_exists ||
            head == e->sym_forall_u || head == e->sym_exists_u) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        /* parametric-defstruct-fn-field-gaps (Gap 1): a fn-typed field
         * `(fn [A...] R)` / `(c-fn [A...] R)` / `(-> A... R)` is a function
         * type, not a type-application.  Without this dispatch the generic
         * type-app loop below recurses into the `[A...]` param vector and
         * mis-parses it as a TupleN literal (a 1-arg fn hits the "tuple type
         * must have 2 to 8 element types" error).  type_expr_from_form has
         * the real fn-type parser; route there directly. */
        if (head == e->sym_fn || head == e->sym_c_fn || head == e->sym_arrow) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        bool has_pipe = false, has_amp = false;
        for (uint32_t i = 0; i < form->as.list.len; i++) {
            Form *item = form->as.list.items[i];
            if (item->tag != F_SYM) continue;
            if (item->as.sym == e->sym_pipe) has_pipe = true;
            if (item->as.sym == e->sym_ampersand) has_amp = true;
        }
        if (has_pipe || has_amp) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        Type *cur = struct_field_type_from_form(e, form->as.list.items[0],
                                                type_params, type_param_kinds, n_type_params);
        if (!cur) return NULL;
        for (uint32_t i = 1; i < form->as.list.len; i++) {
            Type *arg = struct_field_type_from_form(e, form->as.list.items[i],
                                                    type_params, type_param_kinds, n_type_params);
            if (!arg) return NULL;
            Type *next = (Type *)arena_alloc(e->arena, sizeof(Type));
            *next = type_app(e->arena, *cur, *arg, form->span);
            cur = next;
        }
        return cur;
    }
    return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
}

bool elab_struct_type_extract_args(const Type *t, const StructDef *def, Type *out_args) {
    if (!t || !def || def->n_type_params == 0 || !out_args) return false;
    const Type *cur = t;
    uint8_t n_raw = 0;
    Type *raw = (Type *)malloc(def->n_type_params * sizeof(Type));
    if (!raw) return false;
    while (cur && cur->kind == TY_APP && n_raw < def->n_type_params) {
        if (!cur->as.app.arg) break;
        raw[n_raw++] = *cur->as.app.arg;
        cur = cur->as.app.fn;
    }
    bool ok = (cur && cur->kind == TY_STRUCT && cur->as.struct_.def == def &&
               n_raw == def->n_type_params);
    if (ok) {
        for (uint8_t i = 0; i < n_raw; i++) out_args[i] = raw[n_raw - 1 - i];
    }
    free(raw);
    return ok;
}

/* TP6: Unpack a TY_APP chain on an ADT type to recover concrete type arguments.
 * Analogous to elab_struct_type_extract_args but for AdtDef instead of StructDef. */
static bool elab_adt_type_extract_args(const Type *t, const AdtDef *def, Type *out_args) {
    if (!t || !def || def->n_type_params == 0 || !out_args) return false;
    const Type *cur = t;
    uint8_t n_raw = 0;
    Type *raw = (Type *)malloc(def->n_type_params * sizeof(Type));
    if (!raw) return false;
    while (cur && cur->kind == TY_APP && n_raw < def->n_type_params) {
        if (!cur->as.app.arg) break;
        raw[n_raw++] = *cur->as.app.arg;
        cur = cur->as.app.fn;
    }
    bool ok = (cur && cur->kind == TY_ADT && cur->as.adt_.def == def &&
               n_raw == def->n_type_params);
    if (ok) {
        for (uint8_t i = 0; i < n_raw; i++) out_args[i] = raw[n_raw - 1 - i];
    }
    free(raw);
    return ok;
}

/* TP6: Instantiate a field type by substituting TY_TYVAR names with concrete type args.
 * Analogous to struct_field_instantiate_type but uses AdtDef.type_params for name lookup. */
static Type adt_field_instantiate_type(Elab *e, const AdtDef *def, const Type *t,
                                       const Type *type_args) {
    if (!t) return TYPE_UNKNOWN;
    switch (t->kind) {
        case TY_TYVAR: {
            if (!t->as.tyvar_.name) return *t;
            for (uint8_t i = 0; i < def->n_type_params; i++) {
                if (def->type_params[i] &&
                    strcmp(def->type_params[i], t->as.tyvar_.name) == 0) {
                    return type_args[i];
                }
            }
            return *t;
        }
        case TY_APP: {
            Type fn  = adt_field_instantiate_type(e, def, t->as.app.fn,  type_args);
            Type arg = adt_field_instantiate_type(e, def, t->as.app.arg, type_args);
            return type_app(e->arena, fn, arg, (Span){0});
        }
        default:
            return *t;
    }
}

static Type struct_field_instantiate_type(Elab *e, const StructDef *def, const Type *t,
                                          const Type *type_args) {
    if (!t) return TYPE_UNKNOWN;
    switch (t->kind) {
        case TY_TYVAR: {
            uint8_t idx = 0;
            if (t->as.tyvar_.name && struct_type_param_index(def, t->as.tyvar_.name, &idx)) {
                return type_args[idx];
            }
            return *t;
        }
        case TY_APP: {
            Type fn = struct_field_instantiate_type(e, def, t->as.app.fn, type_args);
            Type arg = struct_field_instantiate_type(e, def, t->as.app.arg, type_args);
            return type_app(e->arena, fn, arg, (Span){0});
        }
        case TY_FN: {
            /* make-struct-parametric-fn-field-inference: substitute struct type
             * parameters that appear inside a fn-typed field's signature, so the
             * field's instantiated use-type (validation + codegen) reflects the
             * inferred args rather than leaving the tyvars (which render/lower as
             * a bare int carrier). */
            Type out = *t;
            uint8_t arity = t->as.fn.arity;
            struct Type **new_args = arity
                ? (struct Type **)arena_alloc(e->arena, arity * sizeof(struct Type *))
                : NULL;
            for (uint8_t i = 0; i < arity; i++) {
                Type slot = (t->as.fn.arg_full_types && t->as.fn.arg_full_types[i])
                    ? *t->as.fn.arg_full_types[i]
                    : type_from_kind(t->as.fn.arg_kinds[i]);
                Type inst = struct_field_instantiate_type(e, def, &slot, type_args);
                out.as.fn.arg_kinds[i] = inst.kind;
                struct Type *boxed = (struct Type *)arena_alloc(e->arena, sizeof(Type));
                *boxed = inst;
                new_args[i] = boxed;
            }
            out.as.fn.arg_full_types = new_args;
            {
                Type rslot = t->as.fn.result_full_type
                    ? *t->as.fn.result_full_type
                    : type_from_kind(t->as.fn.result_kind);
                Type rinst = struct_field_instantiate_type(e, def, &rslot, type_args);
                out.as.fn.result_kind = rinst.kind;
                struct Type *rboxed = (struct Type *)arena_alloc(e->arena, sizeof(Type));
                *rboxed = rinst;
                out.as.fn.result_full_type = rboxed;
            }
            return out;
        }
        case TY_UNION: {
            uint8_t n = t->as.union_.n_members;
            Type **members = (Type **)arena_alloc(e->arena, (n ? n : 1) * sizeof(Type *));
            for (uint8_t i = 0; i < n; i++) {
                members[i] = (Type *)arena_alloc(e->arena, sizeof(Type));
                *members[i] = struct_field_instantiate_type(e, def, t->as.union_.members[i], type_args);
            }
            return type_union_build(e->arena, members, n);
        }
        case TY_INTERSECTION: {
            uint8_t n = t->as.intersection_.n_members;
            Type **members = (Type **)arena_alloc(e->arena, (n ? n : 1) * sizeof(Type *));
            for (uint8_t i = 0; i < n; i++) {
                members[i] = (Type *)arena_alloc(e->arena, sizeof(Type));
                *members[i] = struct_field_instantiate_type(e, def, t->as.intersection_.members[i], type_args);
            }
            return type_intersection_build(e->arena, members, n);
        }
        default:
            return *t;
    }
}

static bool struct_field_collect_type_args(const StructDef *def, const Type *expected,
                                           Type actual, Type *type_args, bool *have_type_args) {
    if (!expected || !def) return true;
    switch (expected->kind) {
        case TY_TYVAR: {
            uint8_t idx = 0;
            if (!expected->as.tyvar_.name ||
                !struct_type_param_index(def, expected->as.tyvar_.name, &idx)) {
                return true;
            }
            if (!have_type_args[idx]) {
                type_args[idx] = actual;
                have_type_args[idx] = true;
                return true;
            }
            return type_eq(type_args[idx], actual);
        }
        case TY_APP:
            if (actual.kind != TY_APP || !expected->as.app.fn || !expected->as.app.arg ||
                !actual.as.app.fn || !actual.as.app.arg) {
                return false;
            }
            return struct_field_collect_type_args(def, expected->as.app.fn, *actual.as.app.fn,
                                                  type_args, have_type_args) &&
                   struct_field_collect_type_args(def, expected->as.app.arg, *actual.as.app.arg,
                                                  type_args, have_type_args);
        case TY_FN: {
            /* make-struct-parametric-fn-field-inference: a struct type parameter
             * may appear only inside a fn-typed field (e.g. `(run (fn [A] A))`).
             * Descend into the declared fn type and unify each arg/result slot
             * (which may be a tyvar) against the supplied function value's
             * corresponding slot, so `A` infers from `inc`'s `(fn [int] int)`. */
            if (actual.kind != TY_FN) return false;
            if (expected->as.fn.arity != actual.as.fn.arity) return false;
            for (uint8_t i = 0; i < expected->as.fn.arity; i++) {
                Type exp_arg = (expected->as.fn.arg_full_types && expected->as.fn.arg_full_types[i])
                    ? *expected->as.fn.arg_full_types[i]
                    : type_from_kind(expected->as.fn.arg_kinds[i]);
                Type act_arg = (actual.as.fn.arg_full_types && actual.as.fn.arg_full_types[i])
                    ? *actual.as.fn.arg_full_types[i]
                    : type_from_kind(actual.as.fn.arg_kinds[i]);
                if (!struct_field_collect_type_args(def, &exp_arg, act_arg,
                                                    type_args, have_type_args)) {
                    return false;
                }
            }
            Type exp_res = expected->as.fn.result_full_type
                ? *expected->as.fn.result_full_type
                : type_from_kind(expected->as.fn.result_kind);
            Type act_res = actual.as.fn.result_full_type
                ? *actual.as.fn.result_full_type
                : type_from_kind(actual.as.fn.result_kind);
            return struct_field_collect_type_args(def, &exp_res, act_res,
                                                  type_args, have_type_args);
        }
        case TY_UNION:
        case TY_INTERSECTION:
            return type_eq(*expected, actual);
        default:
            return type_eq(*expected, actual);
    }
}

Type elab_struct_field_use_type(Elab *e, const Type *container_type,
                                const StructDef *def, const StructField *field) {
    if (field->full_type) {
        if (def && def->n_type_params > 0 && container_type) {
            Type *type_args = (Type *)arena_alloc(e->arena, def->n_type_params * sizeof(Type));
            if (elab_struct_type_extract_args(container_type, def, type_args)) {
                return struct_field_instantiate_type(e, def, field->full_type, type_args);
            }
            /* CS1b: a bare TY_STRUCT (without applied type args) for a parameterized
             * struct is the carrier representation: all fields are stored as int64_t.
             * Use TYPE_INT so field-access expressions in carrier-path instance
             * bodies type-check against the scalar carrier type. */
            if (container_type->kind == TY_STRUCT) {
                return TYPE_INT;
            }
        }
        return *field->full_type;
    }
    if (field->kind == TY_REF || field->kind == TY_LREF || field->kind == TY_RC || field->kind == TY_WEAK) {
        Type t = type_from_kind(field->kind);
        if (field->kind == TY_REF || field->kind == TY_LREF) {
            t.as.ref.inner = field->inner_kind;
        } else {
            t.as.rc.inner = field->inner_kind;
            if (field->inner_kind == TY_STRUCT && def) t.as.rc.struct_def = (StructDef *)def;
        }
        return t;
    }
    return type_from_kind(field->kind);
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

/* CONV-S1 (defstruct-as-defadt): true iff every field in an old-syntax
 * defstruct field vector is lowerable to a record-`defadt` field with a
 * byte-identical layout.  As of slice 5 (pointer-field widening) that is:
 *   - a primitive scalar (int / float / bool / cstr / sized numerics), or
 *   - a pointer-kinded field (rc<T> / ref<T> / lref<T> / weak<T> / ptr<void>)
 *     or an `fn` field -- each is an 8-byte carrier slot regardless of the inner
 *     type, so the record-ADT path stores it as a scalar carrier exactly as the
 *     struct path does, drop-glue (rc/ref/weak) and all (slice 2), and the
 *     pre-pass / full-elab lowering decision never disagrees because a pointer's
 *     representation does not depend on the (possibly not-yet-known) inner
 *     type's by-value-ness (the by-value ctor casts an `fn` arg to the int64
 *     carrier, slice 6), or
 *   - a bare user type that resolves to a by-value aggregate (a non-heap,
 *     non-opaque, drop-glue-free struct, or a by-value ADT product), which the
 *     record-ADT path now stores INLINE by value exactly as a struct inlines a
 *     nested struct field.
 * A compound (F_LIST) type, or any parametric / :heap field, still
 * disqualifies so the struct keeps the normal struct path.
 * Mirrors the old-syntax pre-scan (name, then F_TYPE_ANN-wrapped type). */
static bool defstruct_fields_all_primitive(Elab *e, const Form *fields_vec) {
    if (!fields_vec || fields_vec->tag != F_VEC) return false;
    uint32_t n = fields_vec->as.list.len;
    if (n == 0) return false;
    uint32_t i = 0;
    while (i < n) {
        if (fields_vec->as.list.items[i]->tag != F_SYM) return false;  /* field name */
        i++;
        if (i >= n) return false;
        const Form *type_tok = fields_vec->as.list.items[i];
        if (type_tok->tag == F_TYPE_ANN) type_tok = type_tok->as.list.items[0];
        if (type_tok->tag != F_KEYWORD && type_tok->tag != F_SYM)
            return false;  /* F_LIST -> compound/applied type -> not leaf */
        TypeKind k = TY_UNKNOWN, inner = TY_UNKNOWN;
        parse_struct_field_type(type_tok->as.sym->name, type_tok->as.sym->len,
                                &k, &inner);
        switch (k) {
            case TY_INT:   case TY_BOOL:  case TY_FLOAT: case TY_CSTR:
            case TY_INT8:  case TY_INT16: case TY_INT32: case TY_INT64:
            case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
            case TY_FLOAT32: case TY_FLOAT64:
                break;  /* primitive scalar -- ok */
            case TY_RC:   case TY_REF:  case TY_LREF:
            case TY_WEAK: case TY_PTR_VOID: case TY_FN:
                /* slice 5/6: a pointer-kinded or `fn` field is an 8-byte carrier
                 * slot whatever its inner type is, so it lowers like a scalar --
                 * the by-value ADT product already stores such fields as carriers
                 * and synthesises drop glue for the owning (rc/ref/weak) ones
                 * (slice 2); the ctor casts an `fn` arg to the int64 carrier
                 * (slice 6).  The inner type is irrelevant to the lowering
                 * decision, so the pre-pass and full elaboration always agree. */
                break;
            case TY_UNKNOWN: {
                /* slice 4: a bare user type that resolves to an ADT is lowerable
                 * -- a by-value ADT field is inlined, a carrier ADT field is
                 * boxed, and both are handled by the record-ADT codegen.  ADT-ness
                 * is stable between the top-level type pre-pass (which sees an
                 * empty stub) and full elaboration, so the lowering decision the
                 * two passes reach always agrees.  A struct-typed field is NOT
                 * accepted: a struct's by-value-ness is not yet known at pre-pass
                 * time (the stub looks trivially copyable), so admitting it could
                 * make the two passes disagree -- it keeps the struct path.  Note
                 * that with the flag on a leaf-scalar nested struct has itself
                 * lowered to an ADT already, so the common nested case still
                 * lowers; only a genuinely struct-only (non-lowering) field type
                 * holds the outer on the struct path. */
                const Symbol *ts = symtab_intern(e->st,
                    strslice(type_tok->as.sym->name, type_tok->as.sym->len));
                Binding *tb = scope_lookup_type_def(e->scope, ts);
                if (!tb || tb->type.kind != TY_ADT) return false;
                break;  /* ADT field -- ok */
            }
            default:
                return false;  /* rc / ref / ptr / fn / etc. -- not yet */
        }
        i++;
    }
    return true;
}

/* CONV-S1 (defstruct-as-defadt): decide whether a `defstruct` form qualifies for
 * the slice-1 lowering -- flag on, old-syntax single field vector,
 * non-parametric, not :heap / :linear, every field a primitive scalar.  Shared
 * by the top-level type pre-pass (which must then register an ADT stub rather
 * than a struct stub) and elab_defstruct (which performs the rewrite), so they
 * agree on which names become ADTs.  Re-derives the annotation / field shape
 * straight from the form (cheap; the form is small). */
bool defstruct_lowers_to_adt(Elab *e, const Form *call) {
    if (!g_opt_defstruct_as_defadt) return false;
    if (call->tag != F_LIST || call->as.list.len < 3) return false;
    const Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) return false;
    uint32_t idx = 2;
    while (idx < call->as.list.len) {
        const Form *kw = call->as.list.items[idx];
        if (kw->tag != F_KEYWORD) break;
        if (kw->as.sym == e->kw_copy || kw->as.sym == e->kw_move ||
            kw->as.sym == e->kw_no_auto_ctor) { idx++; continue; }
        if (kw->as.sym == e->kw_linear || kw->as.sym == e->kw_heap)
            return false;  /* :linear / :heap keep the struct path in slice 1 */
        break;
    }
    /* A leading all-symbol vector is a type-parameter list -> parametric. */
    if (idx < call->as.list.len && call->as.list.items[idx]->tag == F_VEC) {
        const Form *vec = call->as.list.items[idx];
        bool all_syms = vec->as.list.len > 0;
        for (uint32_t i = 0; i < vec->as.list.len; i++)
            if (vec->as.list.items[i]->tag != F_SYM) { all_syms = false; break; }
        if (all_syms) return false;  /* parametric struct */
    }
    if (idx >= call->as.list.len) return false;
    const Form *fields = call->as.list.items[idx];
    if (fields->tag != F_VEC) return false;  /* new-style field-lists: not slice 1 */
    return defstruct_fields_all_primitive(e, fields);
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
    
    /* Check for optional leading :copy / :move / :linear / :heap annotations.
     * A loop (rather than a single check at index 2) lets :heap combine with the
     * substructural keyword in any order, while remaining backward-compatible
     * with the single-keyword form (the `else break` preserves the old
     * "unrecognised keyword -> fall through to fields/type-params" behaviour). */
    bool is_copy = false;
    bool is_linear = false;
    bool is_heap = false;
    bool no_auto_ctor = false; /* CTOR-V0 */
    uint32_t fields_start_idx = 2;

    while (fields_start_idx < call->as.list.len) {
        Form *kw_form = call->as.list.items[fields_start_idx];
        if (kw_form->tag != F_KEYWORD) break;
        if (kw_form->as.sym == e->kw_copy) {
            is_copy = true;
        } else if (kw_form->as.sym == e->kw_move) {
            is_copy = false;
        } else if (kw_form->as.sym == e->kw_linear) {
            /* LT4: :linear structs are exactly-once (CK_LINEAR). */
            is_linear = true;
        } else if (kw_form->as.sym == e->kw_heap) {
            /* end-to-end-monomorphization: typed-pointer ABI (Vec/Map/Set/...). */
            is_heap = true;
        } else if (kw_form->as.sym == e->kw_no_auto_ctor) {
            /* CTOR-V0: opt out of the auto-bound value-namespace constructor. */
            no_auto_ctor = true;
        } else {
            break;
        }
        fields_start_idx++;
    }
    
    /* Phase TM0: optional type-parameter vector [K V ...] before field definitions.
     * If the next form is a vector containing only symbols (no keyword annotations),
     * treat it as a type-param list; field defs then come as separate list forms.
     * Old syntax [field :type ...] is unchanged (vector contains keywords). */
    const char **type_params_arr = NULL;
    uint8_t n_type_params_v = 0;
    bool new_field_syntax = false;
    const Symbol **field_type_params = NULL;
    Kind *field_type_param_kinds = NULL;

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
            field_type_params = (const Symbol **)arena_alloc(e->arena,
                n_type_params_v * sizeof(Symbol *));
            field_type_param_kinds = (Kind *)arena_alloc(e->arena,
                n_type_params_v * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params_v; pi++) {
                const Symbol *psym = maybe_tp->as.list.items[pi]->as.sym;
                /* Variadic HKT rows: a `^&name` prefix marks a row-kinded type
                 * parameter (kind [*], KIND_TYPEROW) -- it ranges over a row of
                 * types (`#row{...}`), e.g. an ECS Query's component row. The
                 * `^&` is stripped so field types and call-site ascriptions
                 * reference the bare name. Other kinds are still inferred from
                 * field usage by infer_struct_type_param_kinds below. */
                if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '&') {
                    const Symbol *bare = symtab_intern(e->st,
                        strslice(psym->name + 2, psym->len - 2));
                    type_params_arr[pi]      = bare->name;
                    field_type_params[pi]    = bare;
                    field_type_param_kinds[pi] = KIND_TYPEROW;
                } else {
                    type_params_arr[pi]      = psym->name;
                    field_type_params[pi]    = psym;
                    field_type_param_kinds[pi] = KIND_STAR;
                }
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
        /* defstruct-grouped-field-spec-vectors: a field-list element that is
         * itself a vector is a grouped `[name : type]` spec -- flatten it into
         * the surrounding `name`, `: type` token stream so it is equivalent to
         * writing the field inline.  This is the shape a
         * `~@(map (fn [c] `[~c : (T ~c)]) comps)` splice produces, letting one
         * variadic macro build the field list.  Types are never bare vectors
         * (they are keyword/symbol/list, wrapped in F_TYPE_ANN), so a top-level
         * F_VEC element is unambiguously a grouped spec. */
        bool has_grouped = false;
        for (uint32_t i = 0; i < fields_form->as.list.len; i++) {
            if (fields_form->as.list.items[i]->tag == F_VEC) { has_grouped = true; break; }
        }
        if (has_grouped) {
            uint32_t flat_n = 0;
            for (uint32_t i = 0; i < fields_form->as.list.len; i++) {
                Form *it = fields_form->as.list.items[i];
                flat_n += (it->tag == F_VEC) ? it->as.list.len : 1;
            }
            Form **flat = (Form **)arena_alloc(e->arena, (flat_n ? flat_n : 1) * sizeof(Form *));
            uint32_t k = 0;
            for (uint32_t i = 0; i < fields_form->as.list.len; i++) {
                Form *it = fields_form->as.list.items[i];
                if (it->tag == F_VEC) {
                    for (uint32_t j = 0; j < it->as.list.len; j++) flat[k++] = it->as.list.items[j];
                } else {
                    flat[k++] = it;
                }
            }
            fields_form = form_vec(e->arena, fields_form->span, flat, flat_n);
        }
    }

    /* CONV-S1 (defstruct-as-defadt experiment): lower a simple leaf-scalar
     * struct to a single-variant record `defadt`, so it flows through the
     * by-value ADT path.  Slice 1 only fires for an old-syntax, non-parametric,
     * non-:heap, non-:linear struct whose fields are all primitive scalars --
     * the subset that hits none of the record-ADT gaps (rc<ADT> deref,
     * pass-by-ptr, nested by-value fields, drop-glue).  Everything else still
     * elaborates as a struct, even with the flag on.  Rewrite:
     *     (defstruct P [a : int b : int])  ->  (defdata P (P [a : int b : int]))
     * and dispatch to elab_defdata, reusing all the AdtDef machinery.  See
     * docs/upcoming/defstruct-as-defadt-plan.md. */
    if (defstruct_lowers_to_adt(e, call)) {
        experiment_warn_if_used("defstruct-as-defadt");
        /* Constructor variant form: (Name <field-vec>) */
        Form *ctor_items[2] = { name_form, fields_form };
        Form *ctor_form = form_list(e->arena, call->span, ctor_items, 2);
        /* (defdata Name [:copy] (Name <field-vec>)) */
        Form *dd_items[4];
        uint32_t ddn = 0;
        dd_items[ddn++] = form_sym(e->arena, name_form->span, e->sym_defdata);
        dd_items[ddn++] = name_form;
        if (is_copy)
            dd_items[ddn++] = form_keyword(e->arena, name_form->span, e->kw_copy);
        dd_items[ddn++] = ctor_form;
        Form *dd_form = form_list(e->arena, call->span, dd_items, ddn);
        return elab_defdata(e, dd_form);
    }

    /* Phase RF0: allow re-elaboration of forward-declared stub types.
     * MF4: only reuse the existing binding as a stub when its kind matches
     * the kind we're elaborating (TY_STRUCT here).  A same-name GADT
     * binding (TY_ADT) is a separate registration and coexists -- the new
     * struct gets a fresh binding, and type-annotation lookups resolve
     * via the GADT-prefers-struct rule in elab_lookup_type_by_name. */
    bool is_forward_stub = false;
    Binding *existing_b = scope_lookup(e->scope, name);
    if (existing_b) {
        bool same_kind_forward =
            (existing_b->type.kind == TY_STRUCT) && elab_is_forward_type(e, name);
        if (same_kind_forward) {
            /* DS4-2: a forward-registered stub is only re-elaborable while
             * it is still an empty placeholder (n_fields == 0).  Once
             * another defstruct has filled it in -- whether earlier in
             * this file or, more commonly, in an auto-loaded stdlib
             * module -- a second defstruct of the same name would also
             * emit a duplicate `typedef struct <Name>` at codegen.  Reject
             * here so the user gets an elaborator diagnostic instead of a
             * cc error. */
            StructDef *existing_def = existing_b->type.as.struct_.def;
            if (existing_def && existing_def->n_fields > 0) {
                diag_emit(DIAG_ERROR, name_form->span,
                          "defstruct: '%s' is already defined "
                          "(an auto-loaded stdlib module or earlier "
                          "form in this file defines a struct with "
                          "this name; pick a distinct name)",
                          name->name);
                return NULL;
            }
            is_forward_stub = true;
        } else if (existing_b->type.kind == TY_ADT) {
            /* MF4: an existing same-name GADT does not block a new struct.
             * Fall through to register a fresh struct binding alongside the
             * GADT entry in adt_defs[].  Type annotations resolve to the
             * GADT under elab_lookup_type_by_name's prefer-GADT rule. */
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
        def->is_heap = is_heap;
        def->no_auto_ctor = no_auto_ctor; /* CTOR-V0 */
        def->needs_drop_glue = false;
        def->origin_file_id = call->span.file_id;
        /* Phase TM0 */
        def->type_params = type_params_arr;
        def->n_type_params = n_type_params_v;
        b->type.hkt_kind = kind_for_arity(n_type_params_v);
        /* Already in global scope and elab registry from the pre-pass */
    } else {
        def = (StructDef *)arena_alloc(e->arena, sizeof(StructDef));
        memset(def, 0, sizeof(*def));  /* DS5: zero is_opaque and any future bool fields */
        def->name = name->name;
        def->n_fields = actual_n_fields;
        def->fields = (StructField *)arena_alloc(e->arena, actual_n_fields * sizeof(StructField));
        memset(def->fields, 0, actual_n_fields * sizeof(StructField));  /* F8: zero full_type and other fields */
        def->is_copy = is_copy;
        def->is_linear = is_linear; /* LT4 */
        def->is_heap = is_heap;
        def->no_auto_ctor = no_auto_ctor; /* CTOR-V0 */
        /* Phase HKT-P4: record the file that defined this struct. */
        def->origin_file_id = call->span.file_id;
        /* Phase TM0 */
        def->type_params = type_params_arr;
        def->n_type_params = n_type_params_v;

        Type struct_type = type_struct(def);
        struct_type.hkt_kind = kind_for_arity(n_type_params_v);
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
            /* KB-024: track the resolved compound Type (when the field came
             * from an F_LIST) so the :copy diagnostic can print the actual
             * type name even when we don't store it as full_type. */
            Type *compound_type = NULL;

            /* F8 (cross-plan-followups): if the field type is a compound
             * form (F_LIST -- e.g. (exists [a] [(C a)] a), (Vec int)),
             * route through type_expr_from_form to get a full Type and
             * derive the C-level kind from that.  TY_APP, TY_EXISTS,
             * TY_FORALL all lower to int64_t at the C level (opaque
             * heap pointer), so storage layout is unchanged. */
            if (type_name_form->tag == F_LIST) {
                Type *t = (n_type_params_v > 0)
                    ? struct_field_type_from_form(e, type_name_form,
                                                  field_type_params, field_type_param_kinds, n_type_params_v)
                    : type_expr_from_form(e, (Form *)type_name_form,
                                          NULL, NULL, NULL, 0);
                if (!t) return NULL;
                if (n_type_params_v > 0 || t->kind == TY_APP || t->kind == TY_EXISTS ||
                    t->kind == TY_FORALL || t->kind == TY_FN) {
                    full_type = t;
                }
                compound_type = t;
                struct_field_storage_from_type(t, &fkind, &finner);
            } else {
                const char *tname = type_name_form->as.sym->name;
                uint32_t tlen = type_name_form->as.sym->len;
                if (n_type_params_v > 0) {
                    Type *t = struct_field_type_from_form(e, type_name_form,
                        field_type_params, field_type_param_kinds, n_type_params_v);
                    if (!t) return NULL;
                    struct_field_storage_from_type(t, &fkind, &finner);
                    if (struct_type_has_named_tyvar(def, t)) full_type = t;
                } else {
                    parse_struct_field_type(tname, tlen, &fkind, &finner);
                }
                if (fkind == TY_UNKNOWN) {
                    const Symbol *type_sym = symtab_intern(e->st, strslice(tname, tlen));
                    Binding *tb = scope_lookup_type_def(e->scope, type_sym);
                    if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
                        finner = TY_UNKNOWN;
                        fkind = struct_field_user_type_storage(tb, def, &full_type, e->arena);
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
            if (is_copy && typekind_default_copy_kind(fkind) == CK_LINEAR) {
                diag_emit_with_code(DIAG_ERROR, field_type_form->span,
                                    TUR_E0102_LINEAR_COPY,
                                    "cannot copy linear field '%s' -- "
                                    "linear values cannot appear in :copy structs",
                                    field_name_form->as.sym->name);
                return NULL;
            }
            if (is_copy && !typekind_is_copy_for_struct(fkind)) {
                Type *diag_type = full_type ? full_type : compound_type;
                if (diag_type) {
                    diag_emit(DIAG_ERROR, field_type_form->span,
                              "defstruct: field '%s' has non-copy type %s and cannot be used in :copy struct",
                              field_name_form->as.sym->name,
                              type_name(*diag_type));
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
            /* KB-024: track the resolved compound Type (when the field came
             * from an F_LIST) so the :copy diagnostic can print the actual
             * type name even when we don't store it as full_type. */
            Type *compound_type = NULL;

            /* F8 (cross-plan-followups): F_LIST compound field type
             * (e.g. (exists ...), (Vec int)) -- route through
             * type_expr_from_form and store the full Type on the
             * StructField for use sites. */
            if (type_name_form->tag == F_LIST) {
                Type *t = (n_type_params_v > 0)
                    ? struct_field_type_from_form(e, type_name_form,
                                                  field_type_params, field_type_param_kinds, n_type_params_v)
                    : type_expr_from_form(e, (Form *)type_name_form,
                                          NULL, NULL, NULL, 0);
                if (!t) return NULL;
                if (n_type_params_v > 0 || t->kind == TY_APP || t->kind == TY_EXISTS ||
                    t->kind == TY_FORALL || t->kind == TY_FN) {
                    full_type = t;
                }
                compound_type = t;
                struct_field_storage_from_type(t, &fkind, &finner);
            } else {
                const char *tname = type_name_form->as.sym->name;
                uint32_t tlen = type_name_form->as.sym->len;
                if (n_type_params_v > 0) {
                    Type *t = struct_field_type_from_form(e, type_name_form,
                        field_type_params, field_type_param_kinds, n_type_params_v);
                    if (!t) return NULL;
                    struct_field_storage_from_type(t, &fkind, &finner);
                    if (struct_type_has_named_tyvar(def, t)) full_type = t;
                } else {
                    parse_struct_field_type(tname, tlen, &fkind, &finner);
                }

                if (fkind == TY_UNKNOWN) {
                    /* Phase RF0: fall back to user-defined type lookup.  Any struct or
                     * ADT is heap-allocated and stored as an opaque int64_t pointer, so
                     * it is safe to use as a recursive field type without any layout change. */
                    const Symbol *type_sym = symtab_intern(e->st, strslice(tname, tlen));
                    Binding *tb = scope_lookup_type_def(e->scope, type_sym);
                    if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
                        finner = TY_UNKNOWN;
                        fkind = struct_field_user_type_storage(tb, def, &full_type, e->arena);
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
            if (is_copy && typekind_default_copy_kind(fkind) == CK_LINEAR) {
                diag_emit_with_code(DIAG_ERROR, field_type_form->span,
                                    TUR_E0102_LINEAR_COPY,
                                    "cannot copy linear field '%s' -- "
                                    "linear values cannot appear in :copy structs",
                                    field_name_form->as.sym->name);
                return NULL;
            }
            /* :copy struct validation: all fields must be copy */
            if (is_copy && !typekind_is_copy_for_struct(fkind)) {
                Type *diag_type = full_type ? full_type : compound_type;
                if (diag_type) {
                    diag_emit(DIAG_ERROR, field_type_form->span,
                              "defstruct: field '%s' has non-copy type %s and cannot be used in :copy struct",
                              field_name_form->as.sym->name,
                              type_name(*diag_type));
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

    /* TP4: Infer type-param kinds from actual usage in field types.
     * Replaces the position-based struct_type_param_kind heuristic. */
    if (n_type_params_v > 0 && field_type_param_kinds) {
        infer_struct_type_param_kinds(def, field_type_param_kinds);
    }

    /* Phase D: decide pass-by-pointer threshold for non-parameterized structs.
     * For generic structs (n_type_params > 0) the decision is deferred to
     * RegisteredStructApp.pass_by_ptr, computed per-instantiation. */
    if (def->n_type_params == 0 && !def->is_opaque) {
        size_t _d_total = 0;
        for (uint32_t _dfi = 0; _dfi < def->n_fields; _dfi++) {
            int _fsz = type_size_bytes(def->fields[_dfi].kind);
            _d_total += (_fsz > 0) ? (size_t)_fsz : 8;
        }
        def->pass_by_ptr = (_d_total > 16);
    }

    /* CTOR-V0: the auto-bound constructor is provided by routing `(Name args...)`
     * call syntax to make-struct in elab_call (positional and keyword), rather
     * than by synthesizing a `defn` named after the struct.  A same-named value
     * binding was found to interfere with typeclass-instance ABI/dispatch over
     * the struct, and currying a struct-returning constructor cannot work anyway
     * (by-value struct results do not survive the type-erased closure ABI -- see
     * docs/reported/struct-return-through-closure-loses-type.md).  Routing keeps
     * the ergonomic call form without those costs. */

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
 *         (defopaque Name [A ...] :int)         ;; phantom type parameters
 *         (defopaque Name :ptr<void> :linear)   ;; exactly-once resource handle
 *         (defopaque Name :ptr<void> :affine)   ;; at-most-once resource handle
 * Creates a StructDef with is_opaque=true; type_c_name → "int64_t" everywhere.
 * The optional trailing :linear / :affine keyword promotes the newtype to a
 * substructural resource handle (enforced only under -Xlinear / -Xsubstructural;
 * the C ABI is unaffected -- the handle still lowers to int64_t).
 *
 * An optional type-parameter vector [A ...] between the name and the base type
 * declares phantom type parameters: the carrier stays the declared base type
 * (always int64_t at the C level), but the newtype becomes a type constructor
 * of kind '* -> *' (etc.) so it can be spelled `(Name A)` in annotations and
 * track an element/index type at the type level. */
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

    /* Optional phantom type-parameter vector [A ...] immediately after the name.
     * Stored on the StructDef as phantoms; the carrier stays int64_t. */
    const char **type_params_arr = NULL;
    uint8_t n_type_params_v = 0;
    uint32_t base_idx = 2;
    if (call->as.list.items[2]->tag == F_VEC) {
        Form *tp_form = call->as.list.items[2];
        n_type_params_v = (uint8_t)tp_form->as.list.len;
        if (n_type_params_v == 0) {
            diag_emit(DIAG_ERROR, tp_form->span,
                      "defopaque: type-parameter vector cannot be empty");
            return NULL;
        }
        type_params_arr = (const char **)arena_alloc(e->arena,
            n_type_params_v * sizeof(char *));
        for (uint8_t pi = 0; pi < n_type_params_v; pi++) {
            Form *pf = tp_form->as.list.items[pi];
            if (pf->tag != F_SYM) {
                diag_emit(DIAG_ERROR, pf->span,
                          "defopaque: type parameter must be a symbol, e.g. A");
                return NULL;
            }
            type_params_arr[pi] = pf->as.sym->name;
        }
        base_idx = 3;
        /* With a type-param vector consumed, the base type is mandatory. */
        if (call->as.list.len < base_idx + 1) {
            diag_emit(DIAG_ERROR, call->span,
                      "defopaque requires a base type after the type-parameter "
                      "vector: (defopaque Name [A] :int)");
            return NULL;
        }
    }

    /* Optional substructural discipline keyword after the base type. */
    bool opaque_linear = false;
    bool opaque_affine = false;
    if (call->as.list.len >= base_idx + 2) {
        Form *attr = call->as.list.items[base_idx + 1];
        if (attr->tag == F_KEYWORD && attr->as.sym == e->kw_linear) {
            opaque_linear = true;
        } else if (attr->tag == F_KEYWORD && attr->as.sym == e->kw_affine) {
            opaque_affine = true;
        } else {
            diag_emit(DIAG_ERROR, attr->span,
                      "defopaque: unexpected attribute -- expected :linear or :affine");
            return NULL;
        }
    }
    StructDef *def;
    Binding *b;
    /* Phase RF0: the top-level pre-pass forward-registers every defopaque as a
     * stub def (is_copy=true) and binds the type name to type_struct(stub). If
     * we allocated a fresh def here, the type binding -- and every `: Name`
     * annotation resolved through it -- would keep pointing at the stub, so the
     * :linear / :affine discipline would silently never apply. Reuse the stub in
     * place (mirroring elab_defstruct) and refresh the binding's cached type so
     * copy_kind / substruct reflect the declared discipline. */
    if (existing_b && elab_is_forward_type(e, name) &&
            existing_b->type.kind == TY_STRUCT && existing_b->type.as.struct_.def) {
        b = existing_b;
        def = b->type.as.struct_.def;
    } else {
        def = (StructDef *)arena_alloc(e->arena, sizeof(StructDef));
        memset(def, 0, sizeof(*def));  /* DS5: zero all bool / scalar fields by default */
        b = NULL;
    }
    def->name = name->name;
    /* A linear/affine handle is not freely copyable; only a plain opaque is. */
    def->is_copy = !(opaque_linear || opaque_affine);
    def->is_linear = opaque_linear;
    def->is_affine = opaque_affine;
    def->is_opaque = true;
    def->origin_file_id = call->span.file_id;
    /* Phantom type parameters: the carrier is still int64_t, but the newtype is
     * a type constructor (kind '* -> *' etc.) so `(Name A)` annotations parse
     * and the element/index type is tracked at the type level. */
    def->type_params = type_params_arr;
    def->n_type_params = n_type_params_v;
    Type struct_type = type_struct(def);
    struct_type.hkt_kind = kind_for_arity(n_type_params_v);
    if (b) {
        b->type = struct_type;  /* refresh cached copy_kind / substruct + hkt_kind */
    } else {
        b = binding_new(e, name, struct_type, false, true, name_form->span);
        scope_add(&e->global, b);
        elab_register_struct_def(e, def);
    }
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

/* CONV-S0 (struct/ADT convergence): resolve a single constructor field-type
 * form into ctor->fields[fi] (kind/inner_kind/full_type) and ctor->field_forms[fi].
 * Shared by positional-style variants (`(Just :int)`) and record-style variants
 * (`(Circle [radius : float])`); the record path also sets ctor->fields[fi].name.
 * Returns false (diag already emitted) on an unresolvable field type. */
static bool resolve_ctor_field(Elab *e, AdtDef *def, CtorDef *ctor, uint32_t fi,
                               Form *ft_form, const Symbol **tp_syms,
                               uint32_t n_type_params, bool record_style) {
    ctor->fields[fi].full_type = NULL;
    if (ctor->field_forms) ctor->field_forms[fi] = NULL;

    /* TP1: a bare symbol (non-keyword) may be a declared type parameter.
     * E.g. `a` in `(defdata Opt2 [a] (Yep a))`. */
    {
        Type *tv = adt_field_type_from_form(e->arena, ft_form,
                                            def->type_params,
                                            def->n_type_params);
        if (tv) {
            ctor->fields[fi].kind = TY_INT;
            ctor->fields[fi].inner_kind = TY_UNKNOWN;
            ctor->fields[fi].full_type = tv;
            return true;
        }
    }

    /* Applied type constructor or other compound type form in field position. */
    if (ft_form->tag == F_LIST) {
        Type *t = struct_field_type_from_form(e, ft_form, tp_syms,
                                              def->type_param_kinds,
                                              n_type_params);
        if (!t) {
            diag_emit(DIAG_ERROR, ft_form->span,
                      "defdata: could not resolve constructor field type");
            return false;
        }
        TypeKind fkind = TY_UNKNOWN, finner = TY_UNKNOWN;
        struct_field_storage_from_type(t, &fkind, &finner);
        if (fkind == TY_UNKNOWN) { fkind = TY_INT; finner = TY_UNKNOWN; }
        ctor->fields[fi].kind = fkind;
        ctor->fields[fi].inner_kind = finner;
        ctor->fields[fi].full_type = t;
        if (ctor->field_forms) ctor->field_forms[fi] = ft_form;
        if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
            def->needs_drop_glue = true;
        }
        return true;
    }

    /* Positional variants require keyword type names (`:int`); record-style
     * variants (CONV-S0) also accept bare symbol type names (`int`), mirroring
     * defstruct field syntax. */
    bool ok_tag = (ft_form->tag == F_KEYWORD) ||
                  (record_style && ft_form->tag == F_SYM);
    if (!ok_tag) {
        diag_emit(DIAG_ERROR, ft_form->span,
                  "defdata: constructor field type must be a keyword like :int, :bool, :cstr");
        return false;
    }
    const char *tname = ft_form->as.sym->name;
    uint32_t tlen = ft_form->as.sym->len;
    TypeKind fkind, finner;
    parse_struct_field_type(tname, tlen, &fkind, &finner);
    if (fkind == TY_RC && finner == TY_UNKNOWN) {
        /* CONV-S1 (slice 5): rc<Name> over a user struct / record ADT -- carry
         * the inner def on the field's full_type so receivers of the rc field
         * auto-deref through it (mirrors DS3's lookup_rc_inner_struct_def on the
         * struct path).  The field still stores as the TY_RC carrier (the
         * inline-byval gate rejects a TY_RC full_type), so layout is unchanged;
         * only field-access resolution gains the inner layout. */
        Type *rc_full = adt_rc_inner_full_type(e, tname, tlen);
        if (rc_full) {
            ctor->fields[fi].full_type = rc_full;
            finner = (rc_full->kind == TY_RC) ? rc_full->as.rc.inner : finner;
        }
    }
    if (fkind == TY_UNKNOWN) {
        /* Phase RF0: fall back to user-defined type lookup. */
        const Symbol *type_sym = symtab_intern(e->st, strslice(tname, tlen));
        Binding *tb = scope_lookup_type_def(e->scope, type_sym);
        if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
            fkind = TY_INT;
            finner = TY_UNKNOWN;
            /* CONV-S1 (slice 4): a bare field whose type is itself a by-value
             * aggregate (a non-heap/non-opaque drop-glue-free struct, or a
             * by-value ADT product) is stored INLINE by value in the owning
             * by-value product -- the way a struct inlines a nested struct field.
             * Record its full type so codegen lays the field out as the inline
             * aggregate (`tur_adt_<Inner>` / struct C name) instead of boxing it
             * behind the int64 carrier.  A carrier inner (multi-variant ADT,
             * parametric, :heap) keeps full_type NULL and the boxed int64 path. */
            bool inline_byval = false;
            if (tb->type.kind == TY_STRUCT) {
                StructDef *sd = tb->type.as.struct_.def;
                inline_byval = sd && !sd->is_opaque && !sd->is_heap &&
                               !sd->needs_drop_glue;
            } else {
                AdtDef *ad = tb->type.as.adt_.def;
                inline_byval = ad && !ad->needs_drop_glue &&
                               adt_is_byvalue_product(ad);
            }
            if (inline_byval) {
                Type *ft = (Type *)arena_alloc(e->arena, sizeof(Type));
                *ft = tb->type;
                ctor->fields[fi].full_type = ft;
            }
        } else {
            diag_emit(DIAG_ERROR, ft_form->span,
                      "defdata: field has unrecognized type :%s", tname);
            return false;
        }
    }
    ctor->fields[fi].kind = fkind;
    ctor->fields[fi].inner_kind = finner;
    if (ctor->field_forms) ctor->field_forms[fi] = ft_form;
    if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
        def->needs_drop_glue = true;
    }
    return true;
}

/* CONV-S0: a constructor form is record-style when its sole payload is a
 * vector, e.g. `(Circle [radius : float])`.  The vector holds `name : type`
 * pairs exactly like the old-style defstruct field list. */
static bool ctor_form_is_record(const Form *ctor_form) {
    return ctor_form->as.list.len == 2 &&
           ctor_form->as.list.items[1]->tag == F_VEC;
}

/* Phase G0: defdata — define a sum type (ADT)
 * Syntax: (defdata Name [:copy]
 *           (Ctor1)
 *           (Ctor2 :T1 :T2)               ; positional-style variant
 *           (Ctor3 [a : int b : cstr])    ; CONV-S0 record-style variant
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
    /* L6 follow-up C: parallel kind array so `^&name` in a defdata type-param
     * list (e.g. (defdata Frame [^&cols] (Frame :int))) carries KIND_TYPEROW. */
    Kind *parsed_type_param_kinds = NULL;
    if (ctors_start_idx < call->as.list.len &&
        call->as.list.items[ctors_start_idx]->tag == F_VEC) {
        Form *tp_form = call->as.list.items[ctors_start_idx];
        n_type_params = (uint8_t)tp_form->as.list.len;
        if (n_type_params > 0) {
            type_params = (const char **)arena_alloc(e->arena,
                              n_type_params * sizeof(char *));
            parsed_type_param_kinds = (Kind *)arena_alloc(e->arena,
                              n_type_params * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                Form *pf = tp_form->as.list.items[pi];
                if (pf->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, pf->span,
                              "defdata: type parameter must be a symbol, e.g. a, ^f");
                    return NULL;
                }
                const Symbol *psym = pf->as.sym;
                /* L6 follow-up C: '^&name' marks a row-kinded ([*]) parameter. */
                if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '&') {
                    const Symbol *bare = symtab_intern(e->st,
                        strslice(psym->name + 2, psym->len - 2));
                    type_params[pi] = bare->name;
                    parsed_type_param_kinds[pi] = KIND_TYPEROW;
                /* '^^name' marks a kind '* -> * -> *' (binary type constructor)
                 * parameter; '^name' marks a kind '* -> *' parameter.  Mirrors
                 * the defclass handling so a higher-kinded defdata param like
                 * `^f` in (defdata Fix [^f] (Roll (f (Fix f)))) stores the bare
                 * name `f` (so the body's `f` references resolve) and carries the
                 * arrow kind (so applying `f` is well-kinded). */
                } else if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '^') {
                    const Symbol *bare = symtab_intern(e->st,
                        strslice(psym->name + 2, psym->len - 2));
                    type_params[pi] = bare->name;
                    parsed_type_param_kinds[pi] = KIND_ARROW2;
                } else if (psym->len > 1 && psym->name[0] == '^') {
                    const Symbol *bare = symtab_intern(e->st,
                        strslice(psym->name + 1, psym->len - 1));
                    type_params[pi] = bare->name;
                    parsed_type_param_kinds[pi] = KIND_ARROW;
                } else {
                    type_params[pi] = psym->name;
                    parsed_type_param_kinds[pi] = KIND_STAR;
                }
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
        /* TP1: allocate Kind array initialised to KIND_STAR (TP4 refines).
         * L6 follow-up C: seed from parsed_type_param_kinds so `^&` is preserved. */
        if (n_type_params > 0) {
            Kind *tpk = (Kind *)arena_alloc(e->arena, n_type_params * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                tpk[pi] = parsed_type_param_kinds ? parsed_type_param_kinds[pi]
                                                  : KIND_STAR;
            }
            def->type_param_kinds = tpk;
        } else {
            def->type_param_kinds = NULL;
        }
        /* Refresh adt_type from the def so copy_kind reflects is_copy correctly.
         * The pre-pass stub was created with is_copy=false; now that we know the
         * real is_copy flag, regenerate the type and update the binding. */
        adt_type = type_adt(def);
        /* Phase G1/HKT: Apply KIND_ARROW fix so that the kind check can detect
         * when a parameterized defdata type is used in a kind-* slot. */
        adt_type.hkt_kind = kind_for_arity(n_type_params);
        adt_binding->type = adt_type;
        /* Already in global scope and elab registry from the pre-pass */
    } else {
        def = (AdtDef *)arena_alloc(e->arena, sizeof(AdtDef));
        memset(def, 0, sizeof(*def));  /* DS5: zero is_gadt and any future bool fields */
        def->name = name->name;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        def->is_copy = is_copy;
        /* Phase RF1: store type parameters */
        def->type_params = type_params;
        def->n_type_params = n_type_params;
        /* TP1: allocate Kind array initialised to KIND_STAR (TP4 refines).
         * L6 follow-up C: seed from parsed_type_param_kinds so `^&` is preserved. */
        if (n_type_params > 0) {
            Kind *tpk = (Kind *)arena_alloc(e->arena, n_type_params * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                tpk[pi] = parsed_type_param_kinds ? parsed_type_param_kinds[pi]
                                                  : KIND_STAR;
            }
            def->type_param_kinds = tpk;
        } else {
            def->type_param_kinds = NULL;
        }

        /* Pre-register ADT type so constructors can reference it.
         * Phase G1/HKT: Set hkt_kind based on type-parameter count so that
         * elab_defgadt's belt-and-suspenders kind check can detect when a
         * parameterized type constructor is used in a kind-* argument slot. */
        adt_type = type_adt(def);
        adt_type.hkt_kind = kind_for_arity(n_type_params);
        adt_binding = binding_new(e, name, adt_type, false, true, name_form->span);
        scope_add(&e->global, adt_binding);
    }

    /* Record the owning compilation unit so the orphan-instance check can
     * credit instances over this ADT to its defining module (mirrors
     * StructDef.origin_file_id).  Set on both the fresh and reused-stub
     * paths to the real definition's file. */
    def->origin_file_id = name_form->span.file_id;

    /* Build a Symbol** view of the (bare) type-param names so that applied-type
     * constructor fields can be lowered through struct_field_type_from_form,
     * which matches type parameters by interned Symbol identity.  The names in
     * def->type_params are already interned, so re-interning yields the same
     * Symbol* the field forms carry. */
    const Symbol **tp_syms = NULL;
    if (n_type_params > 0) {
        tp_syms = (const Symbol **)arena_alloc(e->arena,
                                               n_type_params * sizeof(Symbol *));
        for (uint8_t pi = 0; pi < n_type_params; pi++) {
            tp_syms[pi] = symtab_intern(e->st,
                strslice(type_params[pi], (uint32_t)strlen(type_params[pi])));
        }
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

        /* CONV-S0: detect record-style variant `(Ctor [a : T b : U ...])`. */
        bool is_record = ctor_form_is_record(ctor_form);
        Form *rec_vec = is_record ? ctor_form->as.list.items[1] : NULL;

        CtorDef *ctor = (CtorDef *)arena_alloc(e->arena, sizeof(CtorDef));
        ctor->name = ctor_name->name;
        ctor->adt = def;
        ctor->tag = ci;
        ctor->result_type_form = NULL; /* Phase G1: NULL for defdata */
        ctor->is_record = is_record;

        uint32_t n_fields;
        /* Names parallel to fields[], populated for record-style variants. */
        const Symbol **rec_field_names = NULL;
        /* Type forms (one per field) to resolve, gathered uniformly for both
         * styles so the field-resolution loop below is shared. */
        Form **field_type_forms = NULL;

        if (is_record) {
            /* Vector holds `name : type` pairs.  The reader collapses `: T`
             * into an F_TYPE_ANN node, so items alternate name, type, name, ...
             * Count name/type pairs first. */
            uint32_t n_items = rec_vec->as.list.len;
            if (n_items == 0) {
                diag_emit(DIAG_ERROR, rec_vec->span,
                          "defdata: record-style variant '%s' field list cannot be empty",
                          ctor_name->name);
                return NULL;
            }
            if (n_items % 2 != 0) {
                diag_emit(DIAG_ERROR, rec_vec->span,
                          "defdata: record-style variant '%s' field list must be "
                          "[name : type ...] pairs", ctor_name->name);
                return NULL;
            }
            n_fields = n_items / 2;
            rec_field_names = (const Symbol **)arena_alloc(e->arena,
                                  n_fields * sizeof(Symbol *));
            field_type_forms = (Form **)arena_alloc(e->arena,
                                  n_fields * sizeof(Form *));
            for (uint32_t fi = 0; fi < n_fields; fi++) {
                Form *name_f = rec_vec->as.list.items[fi * 2];
                Form *type_f = rec_vec->as.list.items[fi * 2 + 1];
                if (name_f->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, name_f->span,
                              "defdata: record-style variant '%s' expected a field "
                              "name symbol", ctor_name->name);
                    return NULL;
                }
                rec_field_names[fi] = name_f->as.sym;
                /* Unwrap `: T` (F_TYPE_ANN) to the bare type form. */
                if (type_f->tag == F_TYPE_ANN)
                    type_f = type_f->as.list.items[0];
                field_type_forms[fi] = type_f;
            }
        } else {
            n_fields = ctor_form->as.list.len - 1;
            if (n_fields > 0) {
                field_type_forms = (Form **)arena_alloc(e->arena,
                                       n_fields * sizeof(Form *));
                for (uint32_t fi = 0; fi < n_fields; fi++)
                    field_type_forms[fi] = ctor_form->as.list.items[1 + fi];
            }
        }

        ctor->n_fields = n_fields;
        ctor->fields = n_fields > 0
            ? (CtorField *)arena_alloc(e->arena, n_fields * sizeof(CtorField))
            : NULL;
        /* F6-1 (cross-plan-followups): stash the raw field-type forms so pattern
         * extraction at match time can recover the declared ADT/struct type. */
        ctor->field_forms = n_fields > 0
            ? (const struct Form **)arena_alloc(e->arena, n_fields * sizeof(const Form *))
            : NULL;

        /* Parse field types (shared between positional and record styles). */
        for (uint32_t fi = 0; fi < n_fields; fi++) {
            if (!resolve_ctor_field(e, def, ctor, fi, field_type_forms[fi],
                                    tp_syms, n_type_params, is_record)) {
                return NULL;
            }
            ctor->fields[fi].name = rec_field_names ? rec_field_names[fi]->name : NULL;

            /* CONV-S5: a :copy ADT requires every variant's payload to be
             * copy-compatible.  A non-copy field (rc/ref/weak/lref ownership)
             * makes the value move-only, which contradicts :copy.  Reject it
             * here, pinpointing the offending field and variant. */
            if (def->is_copy &&
                !typekind_is_copy_for_struct(ctor->fields[fi].kind)) {
                const char *fdesc = ctor->fields[fi].name;
                if (fdesc) {
                    diag_emit(DIAG_ERROR, field_type_forms[fi]->span,
                              "defdata: :copy type '%s' cannot contain non-copy "
                              "field '%s' of variant '%s'",
                              def->name, fdesc, ctor->name);
                } else {
                    diag_emit(DIAG_ERROR, field_type_forms[fi]->span,
                              "defdata: :copy type '%s' cannot contain non-copy "
                              "field %u of variant '%s'",
                              def->name, fi, ctor->name);
                }
                return NULL;
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
            bool any_tyvar = false;
            for (uint8_t fi = 0; fi < arity; fi++) {
                arg_kinds[fi] = ctor->fields[fi].kind;
                if (ctor->fields[fi].full_type && ctor->fields[fi].full_type->kind == TY_TYVAR)
                    any_tyvar = true;
            }
            Type fn_type = type_fn(arg_kinds, arity, TY_ADT);
            /* TS4P1: If any field carries a type-variable full_type, attach the
             * arg_full_types array so elab_call_fn can detect the polymorphic
             * constructor and accept concrete types where TY_INT is expected. */
            if (any_tyvar) {
                Type **aft = (Type **)arena_alloc(e->arena, arity * sizeof(Type *));
                for (uint8_t fi = 0; fi < arity; fi++) {
                    aft[fi] = ctor->fields[fi].full_type; /* may be NULL for non-tyvar fields */
                }
                fn_type.as.fn.arg_full_types = aft;
            }
            Binding *cb = binding_new(e, ctor_name, fn_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        }
    }

    /* Register ADT in elab registry (skip if it was a forward stub -- already registered) */
    if (!is_forward_stub_adt) {
        elab_register_adt_def(e, def);
    }

    /* TP4: refine type_param_kinds based on CtorField.full_type usage */
    infer_type_param_kinds(def);

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

/* TP3: Return the full Type* stored in the skolem binding for name, or NULL.
 * Use when the concrete type is an ADT/struct and the flat TypeKind is insufficient. */
static const Type *gadt_skolem_lookup_type(const SkolemEnv *env, const char *name) {
    if (!env) return NULL;
    for (uint8_t i = 0; i < env->n; i++) {
        if (strcmp(env->bindings[i].name, name) == 0)
            return env->bindings[i].full_type;
    }
    return NULL;
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
        /* Type variable: look up in skolem env — prefer full Type if available (TP3) */
        const Type *full_resolved = gadt_skolem_lookup_type(senv, n);
        if (full_resolved) return *full_resolved;
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

/* TP4: Infer Kind for each type parameter of an ADT/GADT from the declared
 * CtorField.full_type nodes populated by TP1/TP2.
 *
 * Rules (applied across all constructors):
 *   - If a param appears as a direct TY_TYVAR (e.g. field `a`) → KIND_STAR.
 *   - If a param appears as the fn-side of a TY_APP node           → KIND_ARROW.
 *   - Default (param never mentioned in full_type)                 → KIND_STAR.
 *
 * The inferred kinds are written into def->type_param_kinds (allocated by TP1/TP2).
 */
static void infer_type_param_kinds(AdtDef *def) {
    if (!def->type_param_kinds || def->n_type_params == 0) return;

    for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
        const CtorDef *ctor = def->ctors[ci];
        if (!ctor) continue;
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
            const Type *ft = ctor->fields[fi].full_type;
            if (!ft) continue;
            if (ft->kind == TY_TYVAR && ft->as.tyvar_.name) {
                /* Direct param use → KIND_STAR */
                for (uint8_t pi = 0; pi < def->n_type_params; pi++) {
                    if (strcmp(def->type_params[pi], ft->as.tyvar_.name) == 0) {
                        /* Only upgrade if not already KIND_ARROW */
                        if (def->type_param_kinds[pi] == KIND_STAR)
                            def->type_param_kinds[pi] = KIND_STAR; /* no-op; keep default */
                        break;
                    }
                }
            } else if (ft->kind == TY_APP) {
                /* Function-side of TY_APP — param used as a type constructor */
                const Type *fn_side = ft->as.app.fn;
                if (fn_side && fn_side->kind == TY_TYVAR && fn_side->as.tyvar_.name) {
                    for (uint8_t pi = 0; pi < def->n_type_params; pi++) {
                        if (strcmp(def->type_params[pi], fn_side->as.tyvar_.name) == 0) {
                            def->type_param_kinds[pi] = KIND_ARROW;
                            break;
                        }
                    }
                }
            }
        }
    }
}

/* TP4: Recursively fix up hkt_kind on TY_TYVAR nodes embedded in a Type tree.
 * Called after struct type-param kind inference to propagate inferred kinds
 * into the TY_TYVAR nodes stored in StructField.full_type. */
static void fix_tyvar_hkt_kinds(Type *t, const char **type_params,
                                 Kind *type_param_kinds, uint8_t n) {
    if (!t) return;
    switch (t->kind) {
        case TY_TYVAR:
            if (t->as.tyvar_.name) {
                for (uint8_t i = 0; i < n; i++) {
                    if (type_params[i] &&
                        strcmp(type_params[i], t->as.tyvar_.name) == 0) {
                        t->hkt_kind = type_param_kinds[i];
                        break;
                    }
                }
            }
            break;
        case TY_APP:
            fix_tyvar_hkt_kinds(t->as.app.fn,  type_params, type_param_kinds, n);
            fix_tyvar_hkt_kinds(t->as.app.arg, type_params, type_param_kinds, n);
            break;
        default:
            break;
    }
}

/* TP4: Infer Kind for each type parameter of a struct from the declared
 * StructField.full_type nodes.  Analogous to infer_type_param_kinds for AdtDef.
 *   - TY_TYVAR on the fn-side of TY_APP                → KIND_ARROW
 *   - TY_TYVAR used directly (or not seen)              → KIND_STAR (default)
 * Stores the inferred kinds on def->type_param_kinds and fixes up hkt_kind
 * on all TY_TYVAR nodes in stored full_type values. */
static void infer_struct_type_param_kinds(StructDef *def, Kind *field_type_param_kinds) {
    if (!def || !field_type_param_kinds || def->n_type_params == 0) return;

    for (uint32_t fi = 0; fi < def->n_fields; fi++) {
        const Type *ft = def->fields[fi].full_type;
        if (!ft) continue;
        if (ft->kind == TY_APP) {
            const Type *fn_side = ft->as.app.fn;
            if (fn_side && fn_side->kind == TY_TYVAR && fn_side->as.tyvar_.name) {
                for (uint8_t pi = 0; pi < def->n_type_params; pi++) {
                    if (def->type_params[pi] &&
                        strcmp(def->type_params[pi], fn_side->as.tyvar_.name) == 0) {
                        field_type_param_kinds[pi] = KIND_ARROW;
                        break;
                    }
                }
            }
        }
    }

    def->type_param_kinds = field_type_param_kinds;

    /* Fix up hkt_kind on stored TY_TYVAR nodes so they reflect inferred kinds. */
    for (uint32_t fi = 0; fi < def->n_fields; fi++) {
        if (def->fields[fi].full_type) {
            fix_tyvar_hkt_kinds(def->fields[fi].full_type,
                                def->type_params, field_type_param_kinds,
                                def->n_type_params);
        }
    }
}

/* Phase G2: Build a SkolemEnv for a GADT constructor arm.
 * Parses the constructor's result_type_form (e.g. "(Expr int)") against
 * the ADT's type_params (e.g. ["a"]) to produce concrete bindings
 * such as {a → TY_INT}.  Primitive-type args are mapped to their TypeKind;
 * TP3: ADT/struct-type args are now also resolved and stored with their full Type. */
static void gadt_build_skolem_env(Elab *e, SkolemEnv *out, const AdtDef *def,
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
        Type *full_type = NULL;
        /* SZ6: capture a type-level size index when this argument is a Size
         * expression `(Static n)`, `(Add a b)`, or `(Mul a b)`.  Detected
         * structurally by the head symbol, so ordinary ADT applications like
         * `(Foo int)` are never mistaken for size terms. */
        SizeTerm *size_index = NULL;
        if (arg->tag == F_LIST && arg->as.list.len >= 1 &&
                arg->as.list.items[0]->tag == F_SYM) {
            const char *op = arg->as.list.items[0]->as.sym->name;
            if (strcmp(op, "Static") == 0 || strcmp(op, "Add") == 0 ||
                    strcmp(op, "Mul") == 0) {
                size_index = size_term_from_form(e->arena, arg, NULL, NULL);
            }
        }

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
            else {
                /* TP3: may be a named ADT/struct type — resolve via scope */
                Binding *tb = scope_lookup(e->scope, arg->as.sym);
                if (!tb) tb = scope_lookup(&e->global, arg->as.sym);
                if (tb && (tb->type.kind == TY_ADT || tb->type.kind == TY_STRUCT)) {
                    k = tb->type.kind;
                    full_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *full_type = tb->type;
                }
                /* else: type variable in return pos — leave unresolved */
            }
        }
        /* TP3: List form like (Foo int) — ADT/struct application; resolve head */
        else if (arg->tag == F_LIST && arg->as.list.len >= 1) {
            Form *hd = arg->as.list.items[0];
            if (hd->tag == F_SYM) {
                Binding *tb = scope_lookup(e->scope, hd->as.sym);
                if (!tb) tb = scope_lookup(&e->global, hd->as.sym);
                if (tb && (tb->type.kind == TY_ADT || tb->type.kind == TY_STRUCT)) {
                    k = tb->type.kind;
                    full_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *full_type = tb->type;
                } else {
                    k = TY_INT; /* opaque ADT reference fallback */
                }
            } else {
                k = TY_INT;
            }
        }

        if (k != TY_UNKNOWN || size_index) {
            out->bindings[out->n].name       = param_name;
            out->bindings[out->n].kind       = k;
            out->bindings[out->n].full_type  = full_type;
            out->bindings[out->n].size_index = size_index;
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

/* TP1/TP2: If ft_form is a bare symbol matching a declared type parameter,
 * return an arena-allocated TY_TYVAR Type* carrying the parameter name.
 * Returns NULL if the form is not a type-param reference (caller falls back
 * to its normal field-type resolution path).
 * The C-level storage kind remains TY_INT; full_type is elaboration-only. */
static Type *adt_field_type_from_form(Arena *arena, const Form *ft_form,
                                       const char **type_params,
                                       uint8_t n_type_params) {
    if (!ft_form || ft_form->tag != F_SYM) return NULL;
    const char *pname = ft_form->as.sym->name;
    for (uint8_t pi = 0; pi < n_type_params; pi++) {
        if (strcmp(type_params[pi], pname) == 0) {
            Type *tv = (Type *)arena_alloc(arena, sizeof(Type));
            *tv = type_tyvar_named(pname);
            return tv;
        }
    }
    return NULL;
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
    /* range-gadt-typeclass-migration-plan A1: GADTs are enabled by default,
     * so the former -Xgadt gate is gone. */
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
    /* L6 follow-up C: parallel kind array, so a `^&name` row-kinded parameter
     * carries KIND_TYPEROW through to def->type_param_kinds. Without it, the
     * caller-side kind check at (MyGADT #row{...}) sees KIND_STAR and rejects
     * the row as a non-row argument. */
    Kind *parsed_type_param_kinds = NULL;
    if (call->as.list.len >= 3 && call->as.list.items[2]->tag == F_VEC) {
        Form *params_form = call->as.list.items[2];
        n_type_params = (uint8_t)params_form->as.list.len;
        if (n_type_params > 0) {
            type_params = (const char **)arena_alloc(e->arena,
                                                      n_type_params * sizeof(const char *));
            parsed_type_param_kinds = (Kind *)arena_alloc(e->arena,
                                                      n_type_params * sizeof(Kind));
            for (uint8_t i = 0; i < n_type_params; i++) {
                Form *pf = params_form->as.list.items[i];
                if (pf->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, pf->span,
                              "defgadt: type parameter must be a symbol");
                    return NULL;
                }
                const Symbol *psym = pf->as.sym;
                /* L6 follow-up C: '^&name' marks a row-kinded ([*]) parameter.
                 * Strip the `^&` so body and call-site usages reference the
                 * bare name; stash the row kind in parsed_type_param_kinds. */
                if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '&') {
                    const Symbol *bare = symtab_intern(e->st,
                        strslice(psym->name + 2, psym->len - 2));
                    type_params[i] = bare->name;
                    parsed_type_param_kinds[i] = KIND_TYPEROW;
                } else {
                    type_params[i] = psym->name;
                    parsed_type_param_kinds[i] = KIND_STAR;
                }
            }
        }
        ctors_start_idx = 3;
    }

    /* Check for optional :copy / :move annotation after the type-param vector.
     * Mirrors defdata: a GADT marked :copy opts out of affine move tracking so
     * its values can be read repeatedly (e.g. shared range bounds). */
    bool is_copy = false;
    if (ctors_start_idx < call->as.list.len) {
        Form *kw_form = call->as.list.items[ctors_start_idx];
        if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_copy) {
            is_copy = true;
            ctors_start_idx++;
        } else if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_move) {
            is_copy = false;
            ctors_start_idx++;
        }
    }

    if (call->as.list.len <= ctors_start_idx) {
        diag_emit(DIAG_ERROR, call->span,
                  "defgadt: '%s' must have at least one constructor", name->name);
        return NULL;
    }

    /* Phase RF0: allow re-elaboration of forward-declared stub types.
     * MF4: only reuse the existing binding as a stub when its kind matches
     * the kind we're elaborating (TY_ADT here).  A same-name struct
     * binding (TY_STRUCT) does not block the GADT -- per the MF4 design,
     * GADTs and structs occupy separate namespaces and type-annotation
     * lookups prefer the GADT.  Emit a one-shot warning at the defgadt
     * span so the shadowing is visible. */
    bool is_forward_stub_gadt = false;
    Binding *existing_gadt_b = scope_lookup(e->scope, name);
    if (existing_gadt_b) {
        bool same_kind_forward =
            (existing_gadt_b->type.kind == TY_ADT) && elab_is_forward_type(e, name);
        if (same_kind_forward) {
            is_forward_stub_gadt = true;
        } else if (existing_gadt_b->type.kind == TY_STRUCT) {
            /* MF4: GADT shadows an existing struct of the same name.
             * Allow coexistence; warn so the shadowing is visible. */
            diag_emit(DIAG_WARNING, name_form->span,
                      "GADT '%s' shadows existing struct '%s'; uses of "
                      "':%s' in type annotations resolve to the GADT",
                      name->name, name->name, name->name);
            /* fall through: is_forward_stub_gadt stays false, the else
             * branch below registers a fresh GADT binding and AdtDef. */
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
        def->is_copy = is_copy;
        def->needs_drop_glue = false;
        def->is_gadt = true;
        def->type_params = type_params;
        def->n_type_params = n_type_params;
        /* TP2: allocate Kind array initialised to KIND_STAR (TP4 refines).
         * L6 follow-up C: seed from parsed_type_param_kinds so a `^&name`
         * row-kinded parameter is preserved as KIND_TYPEROW. */
        if (n_type_params > 0) {
            Kind *tpk = (Kind *)arena_alloc(e->arena, n_type_params * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                tpk[pi] = parsed_type_param_kinds ? parsed_type_param_kinds[pi]
                                                  : KIND_STAR;
            }
            def->type_param_kinds = tpk;
        } else {
            def->type_param_kinds = NULL;
        }
        adt_type = type_adt(def);
        /* Phase G1/HKT: Apply the same KIND_ARROW fix as the non-stub branch so
         * that kind checks see the correct kind for parameterized GADTs. */
        adt_type.hkt_kind = kind_for_arity(n_type_params);
        adt_binding->type = adt_type;
        /* Already in global scope and elab registry from the pre-pass */
    } else {
        def = (AdtDef *)arena_alloc(e->arena, sizeof(AdtDef));
        memset(def, 0, sizeof(*def));  /* DS5: zero all bool / scalar fields by default */
        def->name = name->name;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        def->is_copy = is_copy;
        def->is_gadt = true;
        def->type_params = type_params;
        def->n_type_params = n_type_params;
        /* TP2: allocate Kind array initialised to KIND_STAR (TP4 refines).
         * L6 follow-up C: seed from parsed_type_param_kinds so a `^&name`
         * row-kinded parameter is preserved as KIND_TYPEROW. */
        if (n_type_params > 0) {
            Kind *tpk = (Kind *)arena_alloc(e->arena, n_type_params * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                tpk[pi] = parsed_type_param_kinds ? parsed_type_param_kinds[pi]
                                                  : KIND_STAR;
            }
            def->type_param_kinds = tpk;
        } else {
            def->type_param_kinds = NULL;
        }

        /* Pre-register ADT type so constructors can reference it.
         * Phase G1/HKT: Set hkt_kind based on type-parameter count so that
         * the belt-and-suspenders kind check can detect when a parameterized
         * GADT is used in a kind-* argument slot of another GADT. */
        adt_type = type_adt(def);
        adt_type.hkt_kind = kind_for_arity(n_type_params);
        adt_binding = binding_new(e, name, adt_type, false, true, name_form->span);
        scope_add(&e->global, adt_binding);
    }

    /* Record the owning compilation unit for the orphan-instance check
     * (mirrors StructDef.origin_file_id). */
    def->origin_file_id = name_form->span.file_id;

    /* Parse each constructor.
     * `ci` is declared outside the loop so the `ctor_parse_error` bail-out
     * path below can truncate `def->n_ctors` to the number of slots we
     * actually filled (every error path bails before `def->ctors[ci]` is
     * assigned, so `ci` is exactly the populated-slot count). */
    uint32_t ci = 0;
    for (; ci < n_ctors; ci++) {
        Form *ctor_form = call->as.list.items[ctors_start_idx + ci];
        if (ctor_form->tag != F_LIST || ctor_form->as.list.len < 1) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defgadt: constructor must be a list form");
            goto ctor_parse_error;
        }
        Form *ctor_name_form = ctor_form->as.list.items[0];
        if (ctor_name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, ctor_name_form->span,
                      "defgadt: constructor name must be a symbol");
            goto ctor_parse_error;
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
            goto ctor_parse_error;
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
                goto ctor_parse_error;
            }
            return_type_form = ann->as.list.items[0];
        } else {
            if ((uint32_t)colon_idx + 1 >= ctor_form->as.list.len) {
                diag_emit(DIAG_ERROR, ctor_form->span,
                          "defgadt: constructor '%s': missing return type after ':'",
                          ctor_name->name);
                goto ctor_parse_error;
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
            goto ctor_parse_error;
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
                goto ctor_parse_error;
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
                        goto ctor_parse_error;
                    }
                    continue;
                }
                /* Unknown -- error */
                diag_emit(DIAG_ERROR, arg->span,
                          "defgadt: unknown type argument '%s' in return type of constructor '%s' "
                          "(must be a type parameter, primitive type, or defined type)",
                          an, ctor_name->name);
                goto ctor_parse_error;
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
        ctor->is_record = false; /* CONV-S0: GADT variants are positional-only */
        /* Phase G2: store raw field-type annotation forms for per-arm resolution */
        ctor->field_forms = n_fields > 0
            ? (const struct Form **)arena_alloc(e->arena, n_fields * sizeof(const Form *))
            : NULL;

        for (uint32_t fi = 0; fi < n_fields; fi++) {
            Form *ft_form = ctor_form->as.list.items[1 + fi];
            TypeKind fkind = gadt_field_typekind_from_form(ft_form);
            ctor->fields[fi].kind = fkind;
            ctor->fields[fi].inner_kind = TY_UNKNOWN;
            ctor->fields[fi].name = NULL; /* CONV-S0: positional */
            /* TP2: populate full_type when the field references a declared type
             * parameter (e.g. `a` in `(MkBox a : (Box a))`).  The C-level kind
             * stays TY_INT; full_type is elaboration-only. */
            ctor->fields[fi].full_type =
                adt_field_type_from_form(e->arena, ft_form,
                                         def->type_params, def->n_type_params);
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

    /* TP4: refine type_param_kinds based on CtorField.full_type usage */
    infer_type_param_kinds(def);

    Expr *out = expr_new(e->arena, EX_DEFGADT, TYPE_NIL, call->span);
    out->as.defgadt_.def = def;
    out->as.defgadt_.binding = adt_binding;
    return out;

ctor_parse_error:
    /* A constructor failed to parse after the AdtDef was pre-registered in
     * scope. The slots `[ci, n_ctors)` are still NULL, but `def->n_ctors`
     * advertises the full declared count, so a later `(match ...)` on this
     * type would dereference a NULL CtorDef and crash (see
     * docs/reported/defgadt-malformed-pattern-segfault.md). Truncate the
     * count to the slots we actually filled. Compilation has already failed
     * (a diagnostic was emitted), so this only prevents the crash. */
    def->n_ctors = ci;
    return NULL;
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
    if (x->type.kind == TY_SESSION) {
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
            /* A constructor slot can be NULL (with a NULL ->name) when its
             * defdata bailed early on a malformed field type, leaving the
             * AdtDef registered with an unfilled slot. Skip such partial
             * slots so lookups recover cleanly instead of dereferencing
             * NULL. */
            CtorDef *ctor = adt->ctors[ci];
            if (!ctor || !ctor->name) continue;
            if (strcmp(ctor->name, name->name) == 0) {
                return ctor;
            }
        }
    }
    return NULL;
}

/* CONV-S3: desugar `(match s (Struct f0 f1 ...) body)` into a sequential let
 * over the existing field accessors and elaborate it.  `pat_idx` is the index
 * of the sole arm's pattern in `call`; `sd` is the matched struct.  Returns the
 * elaborated let, or NULL after emitting a diagnostic. */
static Expr *elab_struct_match_desugar(Elab *e, const Form *call,
                                       uint32_t pat_idx, bool has_guard,
                                       StructDef *sd) {
    Form *pat_form = call->as.list.items[pat_idx];
    if (has_guard) {
        diag_emit(DIAG_ERROR, pat_form->span,
                  "match on struct '%s': when-guards are not supported in a "
                  "single-variant struct match", sd->name);
        return NULL;
    }
    Form *body_form = call->as.list.items[pat_idx + 1];
    Form *scrut_form = call->as.list.items[1];

    /* Resolve one binding-variable form per struct field (or NULL = ignore). */
    Form **field_vars = (Form **)arena_alloc(e->arena,
                            (sd->n_fields ? sd->n_fields : 1) * sizeof(Form *));
    for (uint32_t i = 0; i < sd->n_fields; i++) field_vars[i] = NULL;

    uint32_t n_items = pat_form->as.list.len - 1; /* exclude struct name head */
    bool by_name = (n_items >= 1 && pat_form->as.list.items[1]->tag == F_KEYWORD);

    if (by_name) {
        if (n_items % 2 != 0) {
            diag_emit(DIAG_ERROR, pat_form->span,
                      "match on struct '%s': by-name pattern must be "
                      ":field var pairs", sd->name);
            return NULL;
        }
        for (uint32_t pi = 0; pi < n_items / 2; pi++) {
            Form *kw = pat_form->as.list.items[1 + pi * 2];
            Form *var = pat_form->as.list.items[2 + pi * 2];
            if (kw->tag != F_KEYWORD || var->tag != F_SYM) {
                diag_emit(DIAG_ERROR, kw->span,
                          "match on struct '%s': by-name pattern must be "
                          ":field var pairs", sd->name);
                return NULL;
            }
            int fidx = -1;
            for (uint32_t i = 0; i < sd->n_fields; i++) {
                if (sd->fields[i].name &&
                    strcmp(sd->fields[i].name, kw->as.sym->name) == 0) {
                    fidx = (int)i; break;
                }
            }
            if (fidx < 0) {
                diag_emit(DIAG_ERROR, kw->span,
                          "match on struct '%s': no field '%s'",
                          sd->name, kw->as.sym->name);
                return NULL;
            }
            if (field_vars[fidx]) {
                diag_emit(DIAG_ERROR, kw->span,
                          "match on struct '%s': field '%s' bound more than once",
                          sd->name, kw->as.sym->name);
                return NULL;
            }
            field_vars[fidx] = var;
        }
    } else {
        if (n_items != sd->n_fields) {
            diag_emit(DIAG_ERROR, pat_form->span,
                      "match on struct '%s' expects %u field(s), got %u",
                      sd->name, sd->n_fields, n_items);
            return NULL;
        }
        for (uint32_t i = 0; i < sd->n_fields; i++)
            field_vars[i] = pat_form->as.list.items[1 + i];
    }

    /* Receiver for the field reads.  When the scrutinee is already a bare
     * variable we read fields directly off it (field projection is a borrow,
     * not a move, so multiple reads are fine) -- this avoids copying the struct
     * into a temp, which the by-pointer struct ABI does not support in a let
     * binder.  For a compound scrutinee we bind it to a fresh temp so it is
     * evaluated exactly once. */
    bool scrut_is_var = (scrut_form->tag == F_SYM);
    Form *recv_form = scrut_form;
    const Symbol *wildcard = intern_cstr(e->st, "_");
    uint32_t cap = 2 + sd->n_fields * 2;
    Form **bind_items = (Form **)arena_alloc(e->arena, cap * sizeof(Form *));
    uint32_t bn = 0;
    if (!scrut_is_var) {
        char gbuf[64];
        snprintf(gbuf, sizeof(gbuf), "__tur_smatch_%u", e->next_gensym_id++);
        const Symbol *g_sym = symtab_intern(e->st, strslice(gbuf, (uint32_t)strlen(gbuf)));
        recv_form = form_sym(e->arena, pat_form->span, g_sym);
        bind_items[bn++] = recv_form;
        bind_items[bn++] = scrut_form;
    }
    for (uint32_t i = 0; i < sd->n_fields; i++) {
        Form *var = field_vars[i];
        if (!var || (var->tag == F_SYM && var->as.sym == wildcard)) continue;
        char dotbuf[160];
        int dl = snprintf(dotbuf, sizeof(dotbuf), ".%s", sd->fields[i].name);
        if (dl <= 0 || (size_t)dl >= sizeof(dotbuf)) {
            diag_emit(DIAG_ERROR, pat_form->span,
                      "match on struct '%s': field name too long", sd->name);
            return NULL;
        }
        const Symbol *dot_sym = symtab_intern(e->st, strslice(dotbuf, (uint32_t)dl));
        Form *acc_items[2];
        acc_items[0] = form_sym(e->arena, pat_form->span, dot_sym);
        acc_items[1] = recv_form;
        Form *acc = form_list(e->arena, pat_form->span, acc_items, 2);
        bind_items[bn++] = var;
        bind_items[bn++] = acc;
    }

    /* A struct match that binds no fields (e.g. all `_`) still must elaborate
     * the body; with a bare-var scrutinee there are zero bindings, so emit a
     * trivial `(let [] body)` -- or just the body.  An empty let vector is
     * valid. */
    Form *bind_vec = form_vec(e->arena, pat_form->span, bind_items, bn);

    /* (let <bind_vec> body) */
    const Symbol *let_sym = intern_cstr(e->st, "let");
    Form *let_items[3];
    let_items[0] = form_sym(e->arena, call->span, let_sym);
    let_items[1] = bind_vec;
    let_items[2] = body_form;
    Form *let_form = form_list(e->arena, call->span, let_items, 3);
    return elab_form(e, let_form);
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
    /* Sum-types plan T6: optional `#{NonExhaustive}` opt-out marker.
     * Placed immediately after `match`, before the scrutinee:
     *   (match #{NonExhaustive} scrutinee (Left l) ... )
     * When present, the non-exhaustiveness diagnostic on a known sum/ADT
     * scrutinee is suppressed -- the programmer asserts they have proven
     * exhaustiveness (or coverage of overlap) by other means.  The marker
     * is spliced out so the rest of the function sees an ordinary match.
     * The reader lowers `#{...}` to an F_MAP whose items are the contained
     * symbols (same shape as a defn effect row). */
    bool nonexhaustive_optout = false;
    if (call->as.list.items[1]->tag == F_MAP) {
        const Form *marker = call->as.list.items[1];
        const Symbol *sym_nonexh = intern_cstr(e->st, "NonExhaustive");
        for (uint32_t mi = 0; mi < marker->as.list.len; mi++) {
            if (marker->as.list.items[mi]->tag == F_SYM &&
                marker->as.list.items[mi]->as.sym == sym_nonexh) {
                nonexhaustive_optout = true;
            } else {
                diag_emit(DIAG_ERROR, marker->span,
                          "match: unknown marker in '#{...}'; only "
                          "'#{NonExhaustive}' is recognised here");
                return NULL;
            }
        }
        if (call->as.list.len < 3) {
            diag_emit(DIAG_ERROR, call->span,
                      "match requires a scrutinee after the '#{NonExhaustive}' "
                      "marker: (match #{NonExhaustive} scrutinee pattern body ...)");
            return NULL;
        }
        /* Splice the marker out: rebuild the call form without items[1]. */
        uint32_t new_len = call->as.list.len - 1;
        Form **new_items = (Form **)arena_alloc(e->arena, new_len * sizeof(Form *));
        new_items[0] = call->as.list.items[0];               /* 'match' */
        for (uint32_t k = 2; k < call->as.list.len; k++)
            new_items[k - 1] = call->as.list.items[k];
        call = form_list(e->arena, call->span, new_items, new_len);
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

    /* CONV-S3 (struct/ADT convergence): `match` on a struct value.
     * A struct is the single-variant case of a tagged union, so
     * `(match s (Person name age) body)` is trivially exhaustive.  We detect
     * it syntactically -- the sole arm's pattern head names a struct type --
     * and desugar to a `let` over the existing field accessors, reusing all
     * struct field-access codegen and exhaustiveness-by-construction.  The
     * scrutinee is elaborated exactly once (inside the generated let), so move
     * and linear tracking are unaffected.  Positional `(Person name age)` and
     * by-name `(Person :name n :age a)` binding are both accepted. */
    if (n_arms == 1) {
        Form *pat0 = call->as.list.items[arm_start[0]];
        if (pat0->tag == F_LIST && pat0->as.list.len >= 1 &&
            pat0->as.list.items[0]->tag == F_SYM &&
            !elab_lookup_ctor(e, pat0->as.list.items[0]->as.sym)) {
            const Symbol *head_sym = pat0->as.list.items[0]->as.sym;
            Binding *tb = scope_lookup_type_def(e->scope, head_sym);
            if (tb && tb->type.kind == TY_STRUCT && tb->type.as.struct_.def) {
                Expr *de = elab_struct_match_desugar(e, call, arm_start[0],
                                                     arm_has_guard[0],
                                                     tb->type.as.struct_.def);
                if (de) return de;
                /* de == NULL: a diagnostic was already emitted -> propagate. */
                return NULL;
            }
        }
    }

    /* Elaborate scrutinee */
    Expr *scrutinee = elab_form(e, call->as.list.items[1]);
    if (!scrutinee) return NULL;

    /* IT1: Union type match — when scrutinee is TY_UNION, handle type-narrowing patterns.
     * Pattern syntax: (varname : TypeName) or bare _ / variable for wildcard.
     * Returns early via the union match path. */
    if (scrutinee->type.kind == TY_UNION) {
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
            /* Union match arms carry no guard; initialise the field so readers
             * (e.g. scan_adt_apps_in_expr) do not dereference arena garbage.
             * The arm array is arena-allocated and never zeroed. */
            arms[ai].guard = NULL;

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
                { bool _s = e->in_match_arm; e->in_match_arm = true;
                Expr *body = elab_form(e, body_form);
                e->in_match_arm = _s;
                if (!body) { free(member_covered); return NULL; }
                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type; }
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

                bool _s187 = e->in_match_arm; e->in_match_arm = true;
                Expr *body = elab_form(e, body_form);
                e->in_match_arm = _s187;
                e->scope = saved_scope;
                scope_free(&arm_scope);
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
    if (scrutinee->type.kind == TY_SESSION_OFFER) {
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
                { bool _s = e->in_match_arm; e->in_match_arm = true;
                Expr *body = elab_form(e, body_form);
                e->in_match_arm = _s;
                if (!body) return NULL;
                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type; }
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
                fb->is_match_binding = true;
                if (arm_type.copy_kind == CK_LINEAR) fb->is_linear = true;
                pat->bindings[0] = fb;
                Scope arm_scope;
                scope_init(&arm_scope, e->scope);
                Scope *saved_scope = e->scope;
                e->scope = &arm_scope;
                scope_add(&arm_scope, fb);
                bool _s295 = e->in_match_arm; e->in_match_arm = true;
                Expr *body = elab_form(e, body_form);
                e->in_match_arm = _s295;
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
                bool _s_lit = e->in_match_arm; e->in_match_arm = true;
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
                e->in_match_arm = _s_lit;
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
     * first constructor pattern in the arm list.
     * TP6: Also accept TY_APP chains whose base is a TY_ADT — these carry
     * concrete type arguments from an explicit type annotation. */
    {
        /* Unwrap TY_APP chain to find base type */
        const Type *base = &scrutinee->type;
        while (base && base->kind == TY_APP && base->as.app.fn) {
            base = base->as.app.fn;
        }
        if (base && base->kind == TY_ADT && base->as.adt_.def) {
            /* Scrutinee is a TY_APP(... TY_ADT) chain — valid, nothing to patch */
        } else if (scrutinee->type.kind != TY_ADT || !scrutinee->type.as.adt_.def) {
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
                /* CONV-S1/B2: when the scrutinee is a bare variable whose binding
                 * still carries the int64 default (an untyped param defaulted to
                 * TY_INT), propagate the inferred ADT back onto the binding.  The
                 * signature realiser (elab_fns.c) runs AFTER the body, so it picks
                 * this up and gives the param its ADT type -- making the parameter
                 * and the match body agree on the representation once the by-value
                 * gate flips.  Matching a value with constructor patterns proves it
                 * IS that ADT, so this never mis-types a genuine int.  No-op for
                 * emitted C while the gate is off (type_c_name(TY_ADT) is int64_t).
                 * Only refine a still-default TY_INT binding -- never an explicit
                 * annotation, which already carries its own type kind. */
                if (scrutinee->kind == EX_VAR && scrutinee->as.var.binding &&
                    scrutinee->as.var.binding->type.kind == TY_INT) {
                    scrutinee->as.var.binding->type = type_adt(inferred_adt);
                }
            } else {
                diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                          "match: scrutinee must be an ADT type, got %s",
                          typekind_to_string(scrutinee->type.kind));
                return NULL;
            }
        }
    }

    /* Extract the ADT def, looking through TY_APP chains */
    AdtDef *adt;
    {
        const Type *base = &scrutinee->type;
        while (base && base->kind == TY_APP && base->as.app.fn) {
            base = base->as.app.fn;
        }
        adt = (base && base->kind == TY_ADT) ? base->as.adt_.def
                                              : scrutinee->type.as.adt_.def;
    }

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
    n_match_lin = linear_state_snapshot_bindings(e->scope, &match_lin_bindings,
                                                 &match_lin_before);
    if (n_match_lin > 0) {
        arm_lin_states = (bool **)calloc(n_arms, sizeof(bool *));
        arm_diverges   = (bool  *)calloc(n_arms, sizeof(bool));
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
            if (n_match_lin > 0) {
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            bool _s_wc = e->in_match_arm; e->in_match_arm = true;
            Expr *body = elab_form(e, body_form);
            e->in_match_arm = _s_wc;
            if (!body) { free(covered); return NULL; }
            /* LT1: Capture outer linear state after this arm's body. */
            if (n_match_lin > 0) {
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

            /* CONV-S0: by-name binding for record-style variants.
             * `(Circle :radius r)` binds field `radius` to `r`.  We rewrite the
             * pattern into the equivalent positional form (reordered to field
             * order) so the positional binding loop below is unchanged.  All
             * fields must be listed exactly once; order is free. */
            if (pat_form->as.list.len >= 2 &&
                pat_form->as.list.items[1]->tag == F_KEYWORD) {
                if (!ctor->is_record) {
                    diag_emit(DIAG_ERROR, pat_form->span,
                              "match: constructor '%s' is positional; by-name "
                              "binding (`:field var`) requires a record-style variant",
                              ctor->name);
                    free(covered); return NULL;
                }
                uint32_t n_pairs = (pat_form->as.list.len - 1);
                if (n_pairs % 2 != 0) {
                    diag_emit(DIAG_ERROR, pat_form->span,
                              "match: by-name pattern for '%s' must be "
                              "`:field var` pairs", ctor->name);
                    free(covered); return NULL;
                }
                n_pairs /= 2;
                if (n_pairs != ctor->n_fields) {
                    diag_emit(DIAG_ERROR, pat_form->span,
                              "match: by-name pattern for '%s' must bind all %u "
                              "fields, got %u", ctor->name, ctor->n_fields, n_pairs);
                    free(covered); return NULL;
                }
                Form **pos_items = (Form **)arena_alloc(e->arena,
                                       (ctor->n_fields + 1) * sizeof(Form *));
                pos_items[0] = ctor_name_form;
                for (uint32_t fi = 0; fi < ctor->n_fields; fi++) pos_items[fi + 1] = NULL;
                for (uint32_t pi = 0; pi < n_pairs; pi++) {
                    Form *kw = pat_form->as.list.items[1 + pi * 2];
                    Form *var = pat_form->as.list.items[2 + pi * 2];
                    if (kw->tag != F_KEYWORD || var->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, kw->span,
                                  "match: by-name pattern for '%s' must be "
                                  "`:field var` pairs", ctor->name);
                        free(covered); return NULL;
                    }
                    int fidx = -1;
                    for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                        if (ctor->fields[fi].name &&
                            strcmp(ctor->fields[fi].name, kw->as.sym->name) == 0) {
                            fidx = (int)fi; break;
                        }
                    }
                    if (fidx < 0) {
                        diag_emit(DIAG_ERROR, kw->span,
                                  "match: '%s' is not a field of variant '%s'",
                                  kw->as.sym->name, ctor->name);
                        free(covered); return NULL;
                    }
                    if (pos_items[fidx + 1]) {
                        diag_emit(DIAG_ERROR, kw->span,
                                  "match: field '%s' bound more than once in "
                                  "pattern for '%s'", kw->as.sym->name, ctor->name);
                        free(covered); return NULL;
                    }
                    pos_items[fidx + 1] = var;
                }
                pat_form = form_list(e->arena, pat_form->span, pos_items,
                                     ctor->n_fields + 1);
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
                gadt_build_skolem_env(e, &arm_senv, adt, ctor);
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
                    /* Phase G2: resolve field type using the skolem env.
                     * TP2: if full_type is a named TY_TYVAR, do the skolem
                     * lookup directly — avoids re-parsing and is authoritative. */
                    if (ctor->fields[bi].full_type &&
                            ctor->fields[bi].full_type->kind == TY_TYVAR &&
                            ctor->fields[bi].full_type->as.tyvar_.name) {
                        const char *tvname = ctor->fields[bi].full_type->as.tyvar_.name;
                        TypeKind resolved = gadt_skolem_lookup(&arm_senv, tvname);
                        if (resolved != TY_UNKNOWN) {
                            ftype = type_from_kind(resolved);
                        } else {
                            /* KB-025: the constructor's return-type annotation did
                             * not pin this parameter to a concrete kind.  Refine it
                             * via the scrutinee's instantiation -- a scrutinee of
                             * type (Box t) binds the `a` field to `t`, so a properly
                             * polymorphic arm yields the function's own signature
                             * variable rather than the GADT's internal parameter
                             * name.  When the scrutinee carries no instantiation
                             * (e.g. an unannotated parameter), the field keeps its
                             * unresolved tyvar so the skolem-escape check can fire. */
                            ftype = *ctor->fields[bi].full_type;
                            if (adt->n_type_params > 0) {
                                Type *type_args = (Type *)arena_alloc(e->arena,
                                    adt->n_type_params * sizeof(Type));
                                if (elab_adt_type_extract_args(&scrutinee->type,
                                                               adt, type_args)) {
                                    ftype = adt_field_instantiate_type(e, adt,
                                        ctor->fields[bi].full_type, type_args);
                                }
                            }
                        }
                    } else {
                        ftype = gadt_resolve_type_from_form(e, adt,
                                                            ctor->field_forms[bi], &arm_senv);
                    }
                } else if (ctor->fields[bi].full_type &&
                               ctor->fields[bi].full_type->kind == TY_TYVAR &&
                               adt->n_type_params > 0) {
                    /* TS4P1/TP6: Type-variable field on a defdata constructor.
                     * When the scrutinee has a TY_APP chain (monomorphised instance),
                     * substitute the concrete type argument for the TY_TYVAR name.
                     * This handles the case where ctor->field_forms[bi] is NULL
                     * (because the field was declared as a bare type-variable symbol
                     * like `a` in `(defdata Maybe [a] (Just a))` and the type-variable
                     * branch in defdata parsing used `continue`, skipping field_forms). */
                    Type *type_args = (Type *)arena_alloc(e->arena,
                        adt->n_type_params * sizeof(Type));
                    if (elab_adt_type_extract_args(&scrutinee->type, adt, type_args)) {
                        ftype = adt_field_instantiate_type(e, adt,
                                    ctor->fields[bi].full_type, type_args);
                    } else {
                        ftype = type_from_kind(ctor->fields[bi].kind);
                    }
                } else if (ctor->field_forms && ctor->field_forms[bi]) {
                    /* F6-1 (cross-plan-followups): defdata ctor field with
                     * a stashed type form -- re-parse the type so the binding
                     * carries the declared ADT/struct, not just the C-level
                     * `int` collapsed by parse_struct_field_type for ADT-typed
                     * fields.  Falls back to type_from_kind below if the
                     * re-parse fails (e.g. unknown type).
                     * TP6: If the field carries a TY_TYVAR full_type and the
                     * scrutinee has a TY_APP chain, extract the concrete type
                     * arg and substitute it in. */
                    if (ctor->fields[bi].full_type &&
                            ctor->fields[bi].full_type->kind == TY_TYVAR &&
                            adt->n_type_params > 0) {
                        Type *type_args = (Type *)arena_alloc(e->arena,
                            adt->n_type_params * sizeof(Type));
                        if (elab_adt_type_extract_args(&scrutinee->type, adt, type_args)) {
                            ftype = adt_field_instantiate_type(e, adt,
                                        ctor->fields[bi].full_type, type_args);
                        } else {
                            ftype = type_from_kind(ctor->fields[bi].kind);
                        }
                    } else {
                        Type *resolved = type_expr_from_form(e,
                            (Form *)ctor->field_forms[bi],
                            NULL, NULL, NULL, 0);
                        if (resolved) {
                            ftype = *resolved;
                        } else {
                            ftype = type_from_kind(ctor->fields[bi].kind);
                        }
                    }
                } else {
                    ftype = type_from_kind(ctor->fields[bi].kind);
                }
                Binding *fb = binding_new(e, var_form->as.sym, ftype, false, false,
                                          var_form->span);
                fb->is_match_binding = true;
                /* hkt-cata-function-carrier: a TY_FN value stored in a
                 * PARAMETRIC ADT field (one declared as a bare type variable,
                 * e.g. `a` in `(defdata ExprF [a] (AddF a a))`) is uniformly a
                 * fat box -- a thin fn passed into a tyvar field is boxed via
                 * EX_FN_TO_FAT at construction (elab_call.c ~3310), and a
                 * closure value is already a fat box.  Mark the extracted match
                 * binding `boxed` so an application `(f env)` fat-dispatches
                 * through slot 0 (ER2, emit_expr.c) instead of jumping into the
                 * box as thin code -> SIGSEGV.  A concrete (non-tyvar) TY_FN
                 * field is NOT auto-boxed at construction, so it stays thin. */
                if (fb->type.kind == TY_FN &&
                    ctor->fields[bi].full_type &&
                    ctor->fields[bi].full_type->kind == TY_TYVAR) {
                    fb->is_fat = true;
                }
                /* LT1: Propagate linearity from the field's type to its binding */
                if (ftype.copy_kind == CK_LINEAR) {
                    fb->is_linear = true;
                }
                scope_add(&arm_scope, fb);
                pat->bindings[bi] = fb;
            }

            /* LT1: Restore outer linear state before this arm's body. */
            if (n_match_lin > 0) {
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            bool _s_ctor = e->in_match_arm; e->in_match_arm = true;
            Expr *body = elab_form(e, body_form);
            e->in_match_arm = _s_ctor;

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
            if (body) {
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
            if (body) {
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

            /* Phase G2/HRT: detect skolem escape -- the arm body result is a GADT
             * type variable that the enclosing function does not quantify.
             *
             * An anonymous TY_TYVAR (name == NULL) is always a skolem escape: it
             * is an unresolved GADT field type with no name.
             *
             * KB-025: a *named* TY_TYVAR is only legitimately polymorphic when the
             * surrounding function's signature actually binds it (e.g. `[b :
             * (Box a)] : a`).  When the name is absent from the signature-tyvar
             * set -- as in `[b] : int`, where matching a `(Box a)` binds `x : a`
             * and the arm yields `x` through a concrete `:int` return -- the
             * skolem `a` escapes the match arm. */
            if (adt->is_gadt && body->type.kind == TY_TYVAR) {
                const char *tvname = body->type.as.tyvar_.name;
                bool quantified = false;
                if (tvname) {
                    for (uint8_t si = 0; si < e->n_sig_tyvars; si++) {
                        if (e->sig_tyvars[si] &&
                            strcmp(e->sig_tyvars[si], tvname) == 0) {
                            quantified = true;
                            break;
                        }
                    }
                }
                if (!quantified) {
                    diag_emit(DIAG_ERROR, body_form->span,
                              "match: skolem type variable escapes match arm "
                              "(constructor '%s' of '%s'): the arm body has an "
                              "unresolved GADT type variable as its result type",
                              ctor->name, adt->name);
                    free(covered); return NULL;
                }
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
            if (n_match_lin > 0) {
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
    if (n_match_lin > 0 && arm_lin_states) {
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
    } else {
        /* linear_state_snapshot_bindings always allocates its buffers (cap=16),
         * even when the outer scope holds no linear bindings (n_match_lin == 0).
         * The merge block above only frees them when n_match_lin > 0, so free
         * the snapshot here to avoid a LeakSanitizer-visible leak on every match
         * with no outer linear bindings. */
        free(match_lin_before);
        free(match_lin_bindings);
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
                } else if (nonexhaustive_optout) {
                    /* T6: programmer opted out with #{NonExhaustive}. */
                } else {
                    diag_emit(DIAG_ERROR, call->span,
                              "match: non-exhaustive patterns — constructor '%s' of '%s' not covered",
                              c->name, adt->name);
                    free(covered); return NULL;
                }
            }
        } else {
            for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
                if (!covered[ci] && !nonexhaustive_optout) {
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

    /* Look up the struct binding.  CTOR-V0: use scope_lookup_type_def (which
     * filters for TY_STRUCT/TY_ADT, newest-first) rather than a bare
     * scope_lookup, so a same-named auto-bound constructor (TY_FN, in the value
     * namespace) does not shadow the struct type binding we need here. */
    Binding *struct_binding = scope_lookup_type_def(e->scope, name_form->as.sym);
    if (!struct_binding || struct_binding->type.kind != TY_STRUCT) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "make-struct: '%s' is not a defined struct type",
                  name_form->as.sym->name);
        return NULL;
    }

    StructDef *def = struct_binding->type.as.struct_.def;
    uint32_t n_given = call->as.list.len - 2; /* args after name */
    Type *inferred_type_args = NULL;
    bool *have_type_args = NULL;

    if (def->n_type_params > 0) {
        inferred_type_args = (Type *)arena_alloc(e->arena, def->n_type_params * sizeof(Type));
        have_type_args = (bool *)arena_alloc(e->arena, def->n_type_params * sizeof(bool));
        memset(inferred_type_args, 0, def->n_type_params * sizeof(Type));
        memset(have_type_args, 0, def->n_type_params * sizeof(bool));
    }

    /* M2b: detect keyword form `(make-struct Name :field val :field val ...)`.
     * The form is keyword-style iff the first arg after the struct name is an
     * F_KEYWORD.  Field order may differ from def->fields[]; we reorder into
     * value_forms[] indexed by field position before falling through to the
     * positional elaboration path.  Diagnostics:
     *   TUR-E0292 -- missing field
     *   TUR-E0293 -- duplicate field
     *   TUR-E0294 -- unknown field */
    Form **value_forms = (Form **)arena_alloc(e->arena, def->n_fields * sizeof(Form *));
    bool is_keyword_form = (n_given > 0 && call->as.list.items[2]->tag == F_KEYWORD);
    if (is_keyword_form) {
        /* Pair count must be even. */
        if ((n_given % 2u) != 0u) {
            diag_emit(DIAG_ERROR, call->span,
                      "make-struct '%s': keyword form requires :field value pairs (odd number of args)",
                      def->name);
            return NULL;
        }
        uint32_t n_pairs = n_given / 2u;
        for (uint32_t i = 0; i < def->n_fields; i++) value_forms[i] = NULL;
        for (uint32_t p = 0; p < n_pairs; p++) {
            Form *kw = call->as.list.items[2 + p * 2];
            Form *vf = call->as.list.items[2 + p * 2 + 1];
            if (kw->tag != F_KEYWORD) {
                diag_emit(DIAG_ERROR, kw->span,
                          "TUR-E0299: make-struct '%s': cannot mix positional and "
                          "keyword arguments -- use all positional (%s v1 v2 ...) or "
                          "all keyword (%s :f1 v1 :f2 v2 ...)",
                          def->name, def->name, def->name);
                return NULL;
            }
            const char *kname = kw->as.sym->name;
            uint32_t klen = kw->as.sym->len;
            /* Resolve to field index. */
            uint32_t fi = UINT32_MAX;
            for (uint32_t i = 0; i < def->n_fields; i++) {
                const char *fname = def->fields[i].name;
                if (fname && strlen(fname) == klen && memcmp(fname, kname, klen) == 0) {
                    fi = i; break;
                }
            }
            if (fi == UINT32_MAX) {
                diag_emit(DIAG_ERROR, kw->span,
                          "TUR-E0294: make-struct unknown field '%s' for struct '%s'",
                          kname, def->name);
                return NULL;
            }
            if (value_forms[fi] != NULL) {
                diag_emit(DIAG_ERROR, kw->span,
                          "TUR-E0293: make-struct duplicate field '%s'", kname);
                return NULL;
            }
            value_forms[fi] = vf;
        }
        for (uint32_t i = 0; i < def->n_fields; i++) {
            if (value_forms[i] == NULL) {
                diag_emit(DIAG_ERROR, call->span,
                          "TUR-E0292: make-struct missing field '%s'", def->fields[i].name);
                return NULL;
            }
        }
    } else {
        /* KW-V0: a stray keyword anywhere in an otherwise-positional call is a
         * mixed-form error (the leading arg was positional, so this is not the
         * keyword path).  Catch it explicitly rather than letting it surface as
         * a confusing arity mismatch. */
        for (uint32_t a = 0; a < n_given; a++) {
            if (call->as.list.items[2 + a]->tag == F_KEYWORD) {
                diag_emit(DIAG_ERROR, call->as.list.items[2 + a]->span,
                          "TUR-E0299: make-struct '%s': cannot mix positional and "
                          "keyword arguments -- use all positional (%s v1 v2 ...) or "
                          "all keyword (%s :f1 v1 :f2 v2 ...)",
                          def->name, def->name, def->name);
                return NULL;
            }
        }
        if (n_given != def->n_fields) {
            diag_emit(DIAG_ERROR, call->span,
                      "make-struct '%s': expected %u field value(s), got %u",
                      def->name, def->n_fields, n_given);
            return NULL;
        }
        for (uint32_t i = 0; i < def->n_fields; i++) {
            value_forms[i] = call->as.list.items[2 + i];
        }
    }

    /* Elaborate each field value */
    Expr **field_values = (Expr **)arena_alloc(e->arena, def->n_fields * sizeof(Expr *));
    for (uint32_t i = 0; i < def->n_fields; i++) {
        Expr *fv = elab_form(e, value_forms[i]);
        if (!fv) return NULL;
        field_values[i] = fv;

        if (def->n_type_params > 0 && def->fields[i].full_type) {
            if (!struct_field_collect_type_args(def, def->fields[i].full_type,
                                                fv->type, inferred_type_args, have_type_args)) {
                Type expected = elab_struct_field_use_type(e, &fv->type, def, &def->fields[i]);
                if (fv->type.kind != TY_PTR_VOID && !type_eq(fv->type, expected)) {
                    diag_emit(DIAG_ERROR, value_forms[i]->span,
                              "make-struct '%s': field '%s' expects %s, got %s",
                              def->name, def->fields[i].name,
                              type_name(expected), type_name(fv->type));
                    return NULL;
                }
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
                                         value_forms[i]->span);
            }
        }
    }

    /* SC7: a phantom type parameter (one that appears in no field, e.g. the
     * `A` of `(defstruct Schema [A] (raw :int))`) cannot be inferred from the
     * field values.  Fall back to the return-type-directed expected-type
     * channel: when the make-struct sits under an ascription `(:: e (Schema T))`
     * (or any context that pushed `(Schema T)` onto e->expected_type), pull the
     * missing parameter(s) from that applied type.  This walks the left-nested
     * TY_APP spine `(((Def a) b) c)` and matches the base struct by identity. */
    if (have_type_args && def->n_type_params > 0 && e->expected_type) {
        Type *exp = e->expected_type;
        /* Collect the app-spine arguments (outermost last). */
        Type spine_args[16];
        uint8_t n_spine = 0;
        Type *cur = exp;
        while (cur && cur->kind == TY_APP && n_spine < 16) {
            spine_args[n_spine++] = *cur->as.app.arg;
            cur = cur->as.app.fn;
        }
        if (cur && cur->kind == TY_STRUCT && cur->as.struct_.def == def &&
            n_spine == def->n_type_params) {
            for (uint8_t i = 0; i < def->n_type_params; i++) {
                if (!have_type_args[i]) {
                    /* spine_args is innermost-first reversed: arg for param i is
                     * spine_args[n_spine - 1 - i]. */
                    inferred_type_args[i] = spine_args[n_spine - 1 - i];
                    have_type_args[i] = true;
                }
            }
        }
    }

    /* Build the result type */
    for (uint8_t i = 0; i < def->n_type_params; i++) {
        if (have_type_args && !have_type_args[i]) {
            diag_emit(DIAG_ERROR, name_form->span,
                      "make-struct '%s': could not infer type parameter '%s' from field values "
                      "(it appears in no field; add a type ascription, e.g. (:: (make-struct %s ...) (%s ...)))",
                      def->name, def->type_params[i], def->name, def->name);
            return NULL;
        }
    }
    for (uint32_t i = 0; i < def->n_fields; i++) {
        if (def->fields[i].full_type) {
            Type expected = elab_struct_field_use_type(e, NULL, def, &def->fields[i]);
            if (have_type_args && def->n_type_params > 0) {
                Type ctor_type = type_struct(def);
                ctor_type.hkt_kind = kind_for_arity(def->n_type_params);
                Type applied = ctor_type;
                for (uint8_t tp = 0; tp < def->n_type_params; tp++) {
                    applied = type_app(e->arena, applied, inferred_type_args[tp], call->span);
                }
                expected = elab_struct_field_use_type(e, &applied, def, &def->fields[i]);
            }
            if (field_values[i]->type.kind != TY_PTR_VOID && !type_eq(field_values[i]->type, expected)) {
                diag_emit(DIAG_ERROR, value_forms[i]->span,
                          "make-struct '%s': field '%s' expects %s, got %s",
                          def->name, def->fields[i].name,
                          type_name(expected), type_name(field_values[i]->type));
                return NULL;
            }
        } else {
            /* Scalar fields carry no full_type, so the strict type_eq path above
             * skips them.  Historically that left a hole: a cstr value silently
             * stored into an int field (or vice versa) reinterpreted the pointer
             * bits as a number.  Catch that unambiguous cross-class mismatch --
             * a numeric field given a string value, or a string field given a
             * numeric value -- while still permitting numeric->numeric coercion
             * (int literal into an int8/float field, etc.) which existing call
             * sites rely on. */
            TypeKind want = def->fields[i].kind;
            TypeKind got  = field_values[i]->type.kind;
            /* Only for non-parametric structs: a parametric field's stored kind
             * is the int64 *carrier* for its type variable, into which a cstr
             * (A := cstr) is legitimately bridged -- the guard must not fire
             * there.  Concrete (non-parametric) fields have no such carrier. */
            if (def->n_type_params == 0 &&
                got != TY_PTR_VOID && got != TY_UNKNOWN && want != TY_UNKNOWN) {
                bool want_str = (want == TY_CSTR);
                bool got_str  = (got == TY_CSTR);
                bool want_num = typekind_is_numeric(want);
                bool got_num  = typekind_is_numeric(got);
                if ((want_str && got_num) || (want_num && got_str)) {
                    diag_emit(DIAG_ERROR, value_forms[i]->span,
                              "make-struct '%s': field '%s' expects %s, got %s",
                              def->name, def->fields[i].name,
                              type_name(type_from_kind(want)),
                              type_name(field_values[i]->type));
                    return NULL;
                }
            }
        }
    }
    Type result_type = type_struct(def);
    if (def->n_type_params > 0) {
        result_type = struct_binding->type;
        for (uint8_t i = 0; i < def->n_type_params; i++) {
            result_type = type_app(e->arena, result_type, inferred_type_args[i], call->span);
        }
    }

    Expr *out = expr_new(e->arena, EX_MAKE_STRUCT, result_type, call->span);
    out->as.make_struct_.def = def;
    out->as.make_struct_.field_values = field_values;
    out->as.make_struct_.n_fields = def->n_fields;
    return out;
}

/* CONV-S4: functional update for a single-variant record ADT.  Mirrors the
 * struct lowering in elab_with, but constructs through the auto-bound variant
 * constructor `Ctor`:
 *
 *   (let [G src] (Ctor <f0> <f1> ...))   ; <fi> = override value or (.field G)
 *
 * The ctor is positional in declared field order, so each unchanged field is
 * filled by `(.field G)` (which now resolves on a single-variant record ADT,
 * see the dot-accessor handler in elab_typeclasses.c). */
static Expr *elab_with_record_adt(Elab *e, const Form *call,
                                  const Form *src_form, const Form *ovr_form,
                                  const CtorDef *ctor) {
    Span sp = call->span;
    if (!ctor->adt->is_copy) {
        diag_emit(DIAG_ERROR, call->span,
                  "TUR-E0296: with requires a :copy type -- '%s' is move-only, "
                  "so copying its unchanged fields out of the source would "
                  "consume it. Declare it `(defdata %s :copy ...)` to use with.",
                  ctor->adt->name, ctor->adt->name);
        return NULL;
    }

    /* Map each override field name to its declared field index. */
    Form **ovr_val = (Form **)arena_alloc(e->arena,
        (ctor->n_fields ? ctor->n_fields : 1) * sizeof(Form *));
    for (uint32_t i = 0; i < ctor->n_fields; i++) ovr_val[i] = NULL;
    uint32_t n_pairs = ovr_form->as.list.len / 2u;
    for (uint32_t p = 0; p < n_pairs; p++) {
        Form *fname = ovr_form->as.list.items[p * 2u];
        Form *fval  = ovr_form->as.list.items[p * 2u + 1u];
        if (fname->tag != F_SYM) {
            diag_emit(DIAG_ERROR, fname->span,
                      "with: override field must be a bare field-name symbol");
            return NULL;
        }
        const char *kn = fname->as.sym->name;
        uint32_t klen = fname->as.sym->len;
        uint32_t fi = UINT32_MAX;
        for (uint32_t i = 0; i < ctor->n_fields; i++) {
            const char *dn = ctor->fields[i].name;
            if (dn && strlen(dn) == klen && memcmp(dn, kn, klen) == 0) { fi = i; break; }
        }
        if (fi == UINT32_MAX) {
            diag_emit(DIAG_ERROR, fname->span,
                      "TUR-E0297: with unknown field '%s' for variant '%s'",
                      kn, ctor->name);
            return NULL;
        }
        if (ovr_val[fi]) {
            diag_emit(DIAG_ERROR, fname->span,
                      "TUR-E0298: with duplicate override field '%s'", kn);
            return NULL;
        }
        ovr_val[fi] = fval;
    }

    /* Build (let [G src] (Ctor <f0> ...)). */
    char g_name[32];
    snprintf(g_name, sizeof(g_name), "__with_%u", e->next_id++);
    const Symbol *g_sym = symtab_intern(e->st, strslice(g_name, (uint32_t)strlen(g_name)));
    Form *g_form = form_sym(e->arena, sp, g_sym);

    const Symbol *ctor_sym = symtab_intern(e->st,
        strslice(ctor->name, (uint32_t)strlen(ctor->name)));

    uint32_t cc_n = 1u + ctor->n_fields;
    Form **cc = (Form **)arena_alloc(e->arena, (cc_n ? cc_n : 1) * sizeof(Form *));
    cc[0] = form_sym(e->arena, sp, ctor_sym);
    for (uint32_t i = 0; i < ctor->n_fields; i++) {
        if (ovr_val[i]) {
            cc[1u + i] = ovr_val[i];
        } else {
            char acc[160];
            int al = snprintf(acc, sizeof(acc), ".%s", ctor->fields[i].name);
            const Symbol *acc_sym = symtab_intern(e->st,
                strslice(acc, (al > 0 ? (uint32_t)al : 0)));
            Form *ai[2];
            ai[0] = form_sym(e->arena, sp, acc_sym);
            ai[1] = g_form;
            cc[1u + i] = form_list(e->arena, sp, ai, 2);
        }
    }
    Form *ctor_call = form_list(e->arena, sp, cc, cc_n);

    Form *bind_items[2];
    bind_items[0] = g_form;
    bind_items[1] = (Form *)src_form;
    Form *bind_vec = form_vec(e->arena, sp, bind_items, 2);

    Form *let_items[3];
    let_items[0] = form_sym(e->arena, sp, e->sym_let);
    let_items[1] = bind_vec;
    let_items[2] = ctor_call;
    Form *let_form = form_list(e->arena, sp, let_items, 3);
    return elab_form(e, let_form);
}

/* WITH-V0: functional struct update.
 *
 * Syntax: (with src [field0 val0 field1 val1 ...])
 *
 * Returns a new struct of the same type as `src` with the listed fields
 * overridden and every other field copied from `src`.  Only valid on :copy
 * structs (copying the unchanged fields out of a move-only source would
 * consume it).  Lowers to:
 *
 *   (let [G src]
 *     (make-struct Name <f0> <f1> ...))   ; <fi> = override value or (.fi G)
 *
 * Reusing make-struct gives field-order independence, per-field type checking
 * (the same diagnostic the positional constructor produces), and parametric
 * type inference for free.
 */
Expr *elab_with(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "with requires a source value and an override vector: "
                  "(with src [field value ...])");
        return NULL;
    }
    Form *src_form = call->as.list.items[1];
    Form *ovr_form = call->as.list.items[2];
    if (ovr_form->tag != F_VEC) {
        diag_emit(DIAG_ERROR, ovr_form->span,
                  "with: override list must be a vector [field value ...]");
        return NULL;
    }
    if ((ovr_form->as.list.len % 2u) != 0u) {
        diag_emit(DIAG_ERROR, ovr_form->span,
                  "with: override list must have an even number of forms "
                  "([field value ...] pairs)");
        return NULL;
    }

    /* Elaborate the source once to learn its struct type.  This Expr is used
     * only for type detection and is discarded; the let binding below
     * re-elaborates src_form so it is evaluated exactly once at runtime. */
    Expr *src = elab_form(e, src_form);
    if (!src) return NULL;
    Type st = src->type;
    while (st.kind == TY_APP && st.as.app.fn) st = *st.as.app.fn; /* parametric head */
    /* CONV-S4 (struct/ADT convergence): a single-variant record ADT is a
     * product, so `with` updates it exactly like a struct.  Lower to
     * `(let [G src] (Ctor <f0> <f1> ...))` where each `<fi>` is the override
     * value or `(.field G)` for unchanged fields -- the same shape the struct
     * path below uses, but constructing through the auto-bound variant
     * constructor instead of make-struct.  Field-order independence,
     * per-field type checking, and inference all come from reusing the ctor. */
    if (st.kind == TY_ADT && st.as.adt_.def &&
        adt_is_flat_product(st.as.adt_.def) &&
        st.as.adt_.def->n_ctors == 1 && st.as.adt_.def->ctors[0]->is_record) {
        return elab_with_record_adt(e, call, src_form, ovr_form,
                                    st.as.adt_.def->ctors[0]);
    }
    if (st.kind != TY_STRUCT || !st.as.struct_.def) {
        diag_emit(DIAG_ERROR, src_form->span,
                  "with: source must be a struct value, got %s",
                  type_name(src->type));
        return NULL;
    }
    StructDef *def = st.as.struct_.def;
    if (!def->is_copy) {
        diag_emit(DIAG_ERROR, call->span,
                  "TUR-E0296: with requires a :copy struct -- '%s' is move-only, "
                  "so copying its unchanged fields out of the source would consume "
                  "it. Declare it `(defstruct %s :copy ...)` to use with.",
                  def->name, def->name);
        return NULL;
    }

    /* Map each override field name to its declared field index. */
    Form **ovr_val = (Form **)arena_alloc(e->arena,
        (def->n_fields ? def->n_fields : 1) * sizeof(Form *));
    for (uint32_t i = 0; i < def->n_fields; i++) ovr_val[i] = NULL;
    uint32_t n_pairs = ovr_form->as.list.len / 2u;
    for (uint32_t p = 0; p < n_pairs; p++) {
        Form *fname = ovr_form->as.list.items[p * 2u];
        Form *fval  = ovr_form->as.list.items[p * 2u + 1u];
        if (fname->tag != F_SYM) {
            diag_emit(DIAG_ERROR, fname->span,
                      "with: override field must be a bare field-name symbol");
            return NULL;
        }
        const char *kn = fname->as.sym->name;
        uint32_t klen = fname->as.sym->len;
        uint32_t fi = UINT32_MAX;
        for (uint32_t i = 0; i < def->n_fields; i++) {
            const char *dn = def->fields[i].name;
            if (dn && strlen(dn) == klen && memcmp(dn, kn, klen) == 0) { fi = i; break; }
        }
        if (fi == UINT32_MAX) {
            diag_emit(DIAG_ERROR, fname->span,
                      "TUR-E0297: with unknown field '%s' for struct '%s'",
                      kn, def->name);
            return NULL;
        }
        if (ovr_val[fi]) {
            diag_emit(DIAG_ERROR, fname->span,
                      "TUR-E0298: with duplicate override field '%s'", kn);
            return NULL;
        }
        ovr_val[fi] = fval;
    }

    /* Build the lowered (let [G src] (make-struct Name ...)) form. */
    Span sp = call->span;
    char g_name[32];
    snprintf(g_name, sizeof(g_name), "__with_%u", e->next_id++);
    const Symbol *g_sym = symtab_intern(e->st, strslice(g_name, (uint32_t)strlen(g_name)));
    Form *g_form = form_sym(e->arena, sp, g_sym);

    const Symbol *def_name_sym = symtab_intern(e->st,
        strslice(def->name, (uint32_t)strlen(def->name)));

    uint32_t ms_n = 2u + def->n_fields;
    Form **ms = (Form **)arena_alloc(e->arena, ms_n * sizeof(Form *));
    ms[0] = form_sym(e->arena, sp, e->sym_make_struct);
    ms[1] = form_sym(e->arena, sp, def_name_sym);
    for (uint32_t i = 0; i < def->n_fields; i++) {
        if (ovr_val[i]) {
            ms[2u + i] = ovr_val[i];
        } else {
            /* non-overridden field: (.fieldname G) */
            char acc[160];
            int al = snprintf(acc, sizeof(acc), ".%s", def->fields[i].name);
            const Symbol *acc_sym = symtab_intern(e->st,
                strslice(acc, (al > 0 ? (uint32_t)al : 0)));
            Form *ai[2];
            ai[0] = form_sym(e->arena, sp, acc_sym);
            ai[1] = g_form;
            ms[2u + i] = form_list(e->arena, sp, ai, 2);
        }
    }
    Form *ms_form = form_list(e->arena, sp, ms, ms_n);

    Form *bind_items[2];
    bind_items[0] = g_form;
    bind_items[1] = src_form;
    Form *bind_vec = form_vec(e->arena, sp, bind_items, 2);

    Form *let_items[3];
    let_items[0] = form_sym(e->arena, sp, e->sym_let);
    let_items[1] = bind_vec;
    let_items[2] = ms_form;
    Form *let_form = form_list(e->arena, sp, let_items, 3);

    return elab_form(e, let_form);
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
        /* LT1: a borrow is a non-consuming read.  Elaborating the inner
         * EX_VAR above marked the linear binding consumed (the generic
         * EX_VAR path consumes on use); undo it so a later move/drop/consume
         * of the owner is still valid.  With substructural/linear checking
         * now always-on, omitting this turns `(& r)` ... `(drop! r)` into a
         * spurious use-after-consume (TUR-E0101). */
        if (target->is_linear)
            target->is_linear_consumed = false;
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
        /* LT1: a mutable borrow is also a non-consuming read -- undo the
         * linear-consume the inner EX_VAR applied (see elab_borrow_immut). */
        if (target->is_linear)
            target->is_linear_consumed = false;
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
