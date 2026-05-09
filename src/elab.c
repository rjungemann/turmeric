#include "elab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "diag.h"
#include "types.h"

/* Helper to create a Type from TypeKind. */
static Type type_from_kind(TypeKind k) {
    Type t;
    t.kind = k;
    t.as.fn.arity = 0;
    return t;
}

/* ---- scope ---- */

typedef struct Scope {
    struct Scope *parent;
    Binding     **bindings;
    uint32_t      n;
    uint32_t      cap;
} Scope;

static void scope_init(Scope *s, Scope *parent) {
    s->parent = parent;
    s->bindings = NULL;
    s->n = 0;
    s->cap = 0;
}

static void scope_free(Scope *s) {
    free(s->bindings);
    s->bindings = NULL;
    s->n = s->cap = 0;
}

static void scope_add(Scope *s, Binding *b) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4;
        s->bindings = (Binding **)realloc(s->bindings, s->cap * sizeof(Binding *));
        if (!s->bindings) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    s->bindings[s->n++] = b;
}

static Binding *scope_lookup(Scope *s, const Symbol *name) {
    for (Scope *cur = s; cur; cur = cur->parent) {
        for (uint32_t i = cur->n; i > 0; i--) {
            Binding *b = cur->bindings[i - 1];
            if (b->name == name) return b;
        }
    }
    return NULL;
}

/* ---- elaborator state ---- */

typedef struct Elab {
    Arena       *arena;
    SymbolTable *st;
    Scope       *scope;     /* current */
    Scope        global;
    uint32_t     next_id;

    /* Cached symbols for special-form dispatch. */
    const Symbol *sym_def;
    const Symbol *sym_let;
    const Symbol *sym_if;
    const Symbol *sym_do;
    const Symbol *sym_when;
    const Symbol *sym_unless;
    const Symbol *sym_cond;
    const Symbol *sym_set;
    const Symbol *sym_while;
    const Symbol *sym_defn;     /* Phase 2 */
    const Symbol *sym_fn;       /* Phase 2 */
    const Symbol *sym_extern_c; /* Phase 2 */
    const Symbol *sym_caret_mut;   /* ^mut */
    const Symbol *kw_else;         /* :else (the symbol named "else") */
    const Symbol *kw_derive;       /* :as (the symbol named "as") - for inline-C */
} Elab;

static const Symbol *intern_cstr(SymbolTable *st, const char *s) {
    return symtab_intern(st, strslice(s, (uint32_t)strlen(s)));
}

static void elab_init_state(Elab *e, Arena *arena, SymbolTable *st) {
    e->arena = arena;
    e->st = st;
    scope_init(&e->global, NULL);
    e->scope = &e->global;
    e->next_id = 0;

    e->sym_def       = intern_cstr(st, "def");
    e->sym_let       = intern_cstr(st, "let");
    e->sym_if        = intern_cstr(st, "if");
    e->sym_do        = intern_cstr(st, "do");
    e->sym_when      = intern_cstr(st, "when");
    e->sym_unless    = intern_cstr(st, "unless");
    e->sym_cond      = intern_cstr(st, "cond");
    e->sym_cond      = intern_cstr(st, "cond");
    e->sym_set       = intern_cstr(st, "set!");
    e->sym_while     = intern_cstr(st, "while");
    e->sym_defn      = intern_cstr(st, "defn");
    e->sym_fn        = intern_cstr(st, "fn");
    e->sym_extern_c  = intern_cstr(st, "extern-c");
    e->sym_caret_mut = intern_cstr(st, "^mut");
    e->kw_else       = intern_cstr(st, "else");
    e->kw_derive     = intern_cstr(st, "as");
}

static Binding *binding_new(Elab *e, const Symbol *name, Type type,
                            bool is_mut, bool is_global, Span span) {
    Binding *b = (Binding *)arena_alloc(e->arena, sizeof(Binding));
    b->name = name;
    b->type = type;
    b->is_mut = is_mut;
    b->is_global = is_global;
    b->id = e->next_id++;
    b->span = span;
    return b;
}

/* Forward declarations. */
static Expr *elab_form(Elab *e, Form *f);

/* ---- helpers ---- */

static int form_is_keyword_named(const Form *f, const Symbol *name) {
    return f && f->tag == F_KEYWORD && f->as.sym == name;
}

static int form_is_symbol_named(const Form *f, const Symbol *name) {
    return f && f->tag == F_SYM && f->as.sym == name;
}

static Expr *e_nil(Elab *e, Span span) {
    return expr_new(e->arena, EX_NIL_LIT, TYPE_NIL, span);
}

/* ---- special forms ---- */

static Expr *elab_let(Elab *e, const Form *call) {
    /* (let [b1 i1 b2 i2 ...] body...) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "let requires a binding vector");
        return NULL;
    }
    Form *bindings_form = call->as.list.items[1];
    if (bindings_form->tag != F_VEC) {
        diag_emit(DIAG_ERROR, bindings_form->span,
                  "let bindings must be a vector [name init ...]");
        return NULL;
    }

    /* Parse bindings: walk left-to-right, optional ^mut prefix per entry. */
    LetBinding *binds = NULL;
    uint32_t    n_binds = 0, cap = 0;
    Scope       inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;

    int rc = 0;
    uint32_t i = 0;
    while (i < bindings_form->as.list.len) {
        Form *cur = bindings_form->as.list.items[i];
        bool is_mut = false;
        if (form_is_symbol_named(cur, e->sym_caret_mut)) {
            is_mut = true;
            i++;
            if (i >= bindings_form->as.list.len) {
                diag_emit(DIAG_ERROR, cur->span, "trailing ^mut with no binding name");
                rc = -1; break;
            }
            cur = bindings_form->as.list.items[i];
        }
        if (cur->tag != F_SYM) {
            diag_emit(DIAG_ERROR, cur->span,
                      "let binding name must be a symbol, got %s",
                      cur->tag == F_INT ? "an integer" :
                      cur->tag == F_STR ? "a string"   :
                      cur->tag == F_KEYWORD ? "a keyword" : "non-symbol");
            rc = -1; break;
        }
        const Symbol *name = cur->as.sym;
        Span name_span = cur->span;
        i++;
        if (i >= bindings_form->as.list.len) {
            diag_emit(DIAG_ERROR, cur->span,
                      "let binding for '%s' is missing its initializer",
                      name->name);
            rc = -1; break;
        }
        Form *init_form = bindings_form->as.list.items[i++];
        Expr *init = elab_form(e, init_form);
        if (!init) { rc = -1; break; }

        Binding *b = binding_new(e, name, init->type, is_mut, false, name_span);
        scope_add(&inner, b);

        if (n_binds == cap) {
            cap = cap ? cap * 2 : 4;
            binds = (LetBinding *)realloc(binds, cap * sizeof(LetBinding));
            if (!binds) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
        binds[n_binds].binding = b;
        binds[n_binds].init = init;
        n_binds++;
    }

    Expr *body = NULL;
    if (rc == 0) {
        uint32_t body_count = call->as.list.len - 2;
        if (body_count == 0) {
            body = e_nil(e, call->span);
        } else if (body_count == 1) {
            body = elab_form(e, call->as.list.items[2]);
            if (!body) rc = -1;
        } else {
            Expr **items = (Expr **)arena_alloc(e->arena, body_count * sizeof(Expr *));
            for (uint32_t k = 0; k < body_count; k++) {
                items[k] = elab_form(e, call->as.list.items[2 + k]);
                if (!items[k]) { rc = -1; break; }
            }
            if (rc == 0) {
                body = expr_new(e->arena, EX_DO, items[body_count - 1]->type, call->span);
                body->as.do_.items = items;
                body->as.do_.n = body_count;
            }
        }
    }

    /* Pop scope before returning. */
    e->scope = inner.parent;
    scope_free(&inner);

    if (rc != 0) { free(binds); return NULL; }

    Expr *out = expr_new(e->arena, EX_LET, body->type, call->span);
    LetBinding *bcopy = (LetBinding *)arena_alloc(e->arena, n_binds * sizeof(LetBinding));
    memcpy(bcopy, binds, n_binds * sizeof(LetBinding));
    free(binds);
    out->as.let_.bindings = bcopy;
    out->as.let_.n = n_binds;
    out->as.let_.body = body;
    return out;
}

static Expr *elab_do(Elab *e, const Form *call) {
    /* (do body...) — value of last expr; (do) is nil. */
    uint32_t n = call->as.list.len - 1;
    if (n == 0) return e_nil(e, call->span);

    Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
    for (uint32_t i = 0; i < n; i++) {
        items[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!items[i]) return NULL;
    }
    Expr *out = expr_new(e->arena, EX_DO, items[n - 1]->type, call->span);
    out->as.do_.items = items;
    out->as.do_.n = n;
    return out;
}

static Expr *elab_if(Elab *e, const Form *call) {
    if (call->as.list.len != 3 && call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "if expects (if cond then) or (if cond then else); got %u argument(s)",
                  call->as.list.len - 1);
        return NULL;
    }
    Expr *cond = elab_form(e, call->as.list.items[1]);
    if (!cond) return NULL;
    if (!type_eq(cond->type, TYPE_BOOL)) {
        diag_emit(DIAG_ERROR, cond->span,
                  "if condition must be bool, got %s", type_name(cond->type));
        return NULL;
    }
    Expr *then_ = elab_form(e, call->as.list.items[2]);
    if (!then_) return NULL;
    Expr *else_ = NULL;
    Type result_t = TYPE_NIL;
    if (call->as.list.len == 4) {
        else_ = elab_form(e, call->as.list.items[3]);
        if (!else_) return NULL;
        if (!type_eq(then_->type, else_->type)) {
            diag_emit(DIAG_ERROR, call->span,
                      "if branches have mismatched types: then=%s else=%s",
                      type_name(then_->type), type_name(else_->type));
            return NULL;
        }
        result_t = then_->type;
    }
    /* If no else, the if is a statement-style branch with type nil
     * (matches Clojure's behavior of returning nil for a missing else). */
    Expr *out = expr_new(e->arena, EX_IF, result_t, call->span);
    out->as.if_.cond = cond;
    out->as.if_.then_ = then_;
    out->as.if_.else_or_null = else_;
    return out;
}

/* (when c b...) → (if c (do b...) nil) — built directly without Form rewriting. */
static Expr *elab_when_unless(Elab *e, const Form *call, bool inverted) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "%s requires a condition",
                  inverted ? "unless" : "when");
        return NULL;
    }
    Expr *cond = elab_form(e, call->as.list.items[1]);
    if (!cond) return NULL;
    if (!type_eq(cond->type, TYPE_BOOL)) {
        diag_emit(DIAG_ERROR, cond->span, "%s condition must be bool, got %s",
                  inverted ? "unless" : "when", type_name(cond->type));
        return NULL;
    }
    uint32_t n = call->as.list.len - 2;
    Expr *body;
    if (n == 0) body = e_nil(e, call->span);
    else if (n == 1) {
        body = elab_form(e, call->as.list.items[2]);
        if (!body) return NULL;
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
        for (uint32_t i = 0; i < n; i++) {
            items[i] = elab_form(e, call->as.list.items[2 + i]);
            if (!items[i]) return NULL;
        }
        body = expr_new(e->arena, EX_DO, items[n - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n;
    }

    Expr *out = expr_new(e->arena, EX_IF, TYPE_NIL, call->span);
    out->as.if_.cond = cond;
    if (inverted) {
        /* (unless c b...) → if cond is FALSE, run body. Swap then/else. */
        out->as.if_.then_ = e_nil(e, call->span);
        out->as.if_.else_or_null = body;
    } else {
        out->as.if_.then_ = body;
        out->as.if_.else_or_null = NULL;
    }
    return out;
}

static Expr *elab_cond(Elab *e, const Form *call) {
    /* (cond t1 e1 t2 e2 ... :else efinal) — desugars to nested if. */
    uint32_t n = call->as.list.len - 1;
    if ((n & 1) != 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "cond expects pairs of (test expr); got %u clauses", n);
        return NULL;
    }
    /* Build right-to-left. */
    Expr *acc = e_nil(e, call->span);
    Type acc_t = TYPE_NIL;
    for (int i = (int)n - 2; i >= 0; i -= 2) {
        Form *test = call->as.list.items[1 + i];
        Form *body = call->as.list.items[1 + i + 1];

        if (form_is_keyword_named(test, e->kw_else)) {
            acc = elab_form(e, body);
            if (!acc) return NULL;
            acc_t = acc->type;
            continue;
        }

        Expr *cond = elab_form(e, test);
        if (!cond) return NULL;
        if (!type_eq(cond->type, TYPE_BOOL)) {
            diag_emit(DIAG_ERROR, cond->span,
                      "cond test must be bool, got %s", type_name(cond->type));
            return NULL;
        }
        Expr *then_ = elab_form(e, body);
        if (!then_) return NULL;

        /* If this is the first clause (right-most we processed), it sets
         * acc_t. Subsequent clauses must match. */
        if (acc_t.kind == TY_NIL && acc->kind == EX_NIL_LIT) {
            acc_t = then_->type;
            /* allow plain (cond ...) without :else: result type is then_'s type
             * if all branches happen to match; otherwise we'd need a "maybe"
             * type which we don't have. For phase 1, require :else when
             * branches return non-nil values. */
            if (then_->type.kind != TY_NIL) {
                /* leave acc as nil; this is the implicit fallthrough branch */
            }
        }
        if (!type_eq(then_->type, acc_t)) {
            diag_emit(DIAG_ERROR, then_->span,
                      "cond branch type %s does not match earlier branch type %s",
                      type_name(then_->type), type_name(acc_t));
            return NULL;
        }

        Expr *out = expr_new(e->arena, EX_IF, acc_t, test->span);
        out->as.if_.cond = cond;
        out->as.if_.then_ = then_;
        out->as.if_.else_or_null = acc;
        acc = out;
    }
    return acc;
}

static Expr *elab_set(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "set! takes (set! name value)");
        return NULL;
    }
    Form *target = call->as.list.items[1];
    if (target->tag != F_SYM) {
        diag_emit(DIAG_ERROR, target->span, "set! target must be a symbol");
        return NULL;
    }
    Binding *b = scope_lookup(e->scope, target->as.sym);
    if (!b) {
        diag_emit(DIAG_ERROR, target->span,
                  "set!: '%s' is not bound", target->as.sym->name);
        return NULL;
    }
    if (!b->is_mut) {
        diag_emit(DIAG_ERROR, target->span,
                  "set!: '%s' is immutable; use ^mut at the binding site to allow it",
                  b->name->name);
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;
    if (!type_eq(value->type, b->type)) {
        diag_emit(DIAG_ERROR, value->span,
                  "set!: value type %s does not match binding type %s",
                  type_name(value->type), type_name(b->type));
        return NULL;
    }

    Expr *out = expr_new(e->arena, EX_SET, TYPE_NIL, call->span);
    out->as.set_.target = b;
    out->as.set_.value = value;
    return out;
}

static Expr *elab_while(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "while requires a condition");
        return NULL;
    }
    Expr *cond = elab_form(e, call->as.list.items[1]);
    if (!cond) return NULL;
    if (!type_eq(cond->type, TYPE_BOOL)) {
        diag_emit(DIAG_ERROR, cond->span,
                  "while condition must be bool, got %s", type_name(cond->type));
        return NULL;
    }
    uint32_t n = call->as.list.len - 2;
    Expr *body;
    if (n == 0) body = e_nil(e, call->span);
    else if (n == 1) {
        body = elab_form(e, call->as.list.items[2]);
        if (!body) return NULL;
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
        for (uint32_t i = 0; i < n; i++) {
            items[i] = elab_form(e, call->as.list.items[2 + i]);
            if (!items[i]) return NULL;
        }
        body = expr_new(e->arena, EX_DO, TYPE_NIL, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n;
    }
    Expr *out = expr_new(e->arena, EX_WHILE, TYPE_NIL, call->span);
    out->as.while_.cond = cond;
    out->as.while_.body = body;
    return out;
}

/* Phase 2: defn — (defn name [param1 param2 ...] : return-type body...)
 * For now, we only support : int return type annotation. Param types are
 * inferred from usage. */
static Expr *elab_defn(Elab *e, const Form *call) {
    /* Minimum: (defn name []) */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defn requires (defn name [params...] body...)");
        return NULL;
    }

    /* Parse name */
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defn name must be a symbol");
        return NULL;
    }
    Binding *existing = scope_lookup(e->scope, name_f->as.sym);
    if (existing) {
        /* Allow forward-declared bindings from pass 1 to be redefined */
        /* Forward declarations have TY_FN type (from pass 1) */
        if (existing->type.kind == TY_FN && existing->is_global) {
            /* This is a forward declaration - proceed with the real definition */
        } else {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defn: '%s' is already defined", name_f->as.sym->name);
            return NULL;
        }
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[2];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defn: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* Parse params */
    Binding **params = NULL;
    uint8_t n_params = 0;
    TypeKind param_kinds[MAX_FN_ARITY];

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "defn: parameter name must be a symbol");
            free(params);
            return NULL;
        }
        if (n_params >= MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, p->span,
                      "defn: too many parameters (max %d)", MAX_FN_ARITY);
            free(params);
            return NULL;
        }
        /* For phase 2, all params are int by default */
        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation and body */
    TypeKind return_kind = TY_NIL;
    uint32_t body_start = 3;

    /* Check for : return-type annotation */
    if (call->as.list.len >= 4) {
        Form *ret_f = call->as.list.items[3];
        if (ret_f->tag == F_KEYWORD) {
            /* : int, : bool, etc. */
            const Symbol *kw = ret_f->as.sym;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                return_kind = TY_INT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                return_kind = TY_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                return_kind = TY_NIL;
            } else {
                diag_emit(DIAG_ERROR, ret_f->span,
                          "defn: unsupported return type keyword :%s",
                          kw->name);
                return NULL;
            }
            body_start = 4;
        }
    }

    /* Elaborate body */
    if (call->as.list.len < body_start + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "defn: missing body");
        return NULL;
    }

    /* Push a new scope for the function body with params bound */
    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    for (uint8_t i = 0; i < n_params; i++) {
        scope_add(&inner, params[i]);
    }

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;
    if (n_body == 1) {
        body = elab_form(e, call->as.list.items[body_start]);
        if (!body) { e->scope = inner.parent; scope_free(&inner); return NULL; }
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body; i++) {
            items[i] = elab_form(e, call->as.list.items[body_start + i]);
            if (!items[i]) { e->scope = inner.parent; scope_free(&inner); return NULL; }
        }
        body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n_body;
    }

    /* Pop scope */
    e->scope = inner.parent;
    scope_free(&inner);

    /* Create function type */
    TypeKind arg_kinds[MAX_FN_ARITY];
    for (uint8_t i = 0; i < n_params; i++) {
        arg_kinds[i] = TY_INT;  /* All int for phase 2 */
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);

    /* Create Binding for the function */
    Binding *b = binding_new(e, name_f->as.sym, fn_type, false, true, name_f->span);
    /* If there's a forward-declared binding (TY_FN from pass 1), replace it */
    if (existing && existing->type.kind == TY_FN) {
        /* Replace the forward declaration with the real binding */
        /* Find the forward declaration in the scope and replace it */
        for (uint32_t i = 0; i < e->global.n; i++) {
            if (e->global.bindings[i] == existing) {
                e->global.bindings[i] = b;
                break;
            }
        }
    } else {
        scope_add(&e->global, b);
    }

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    fd->binding = b;
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = false;
    /* Store param types for codegen */
    fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint8_t i = 0; i < n_params; i++) {
        fd->param_types[i] = TYPE_INT;
    }

    Expr *out = expr_new(e->arena, EX_FN_DEF, fn_type, call->span);
    out->as.fn_def_.fn = fd;
    return out;
}

/* Phase 2: fn — (fn [param1 param2 ...] body...) — no capture for phase 2
 * Lifts to a static function. For now, we require a return type annotation.
 * Example: (fn [x y] :int (+ x y)) */
static Expr *elab_fn(Elab *e, const Form *call) {
    /* Minimum: (fn [params...] body...) */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "fn requires (fn [params...] body...)");
        return NULL;
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[1];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "fn: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* Parse params */
    Binding **params = NULL;
    uint8_t n_params = 0;
    TypeKind param_kinds[MAX_FN_ARITY];

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "fn: parameter name must be a symbol");
            free(params);
            return NULL;
        }
        if (n_params >= MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, p->span,
                      "fn: too many parameters (max %d)", MAX_FN_ARITY);
            free(params);
            return NULL;
        }
        /* For phase 2, all params are int by default */
        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation and body */
    TypeKind return_kind = TY_NIL;
    uint32_t body_start = 2;

    /* Check for : return-type annotation */
    if (call->as.list.len >= 3) {
        Form *ret_f = call->as.list.items[2];
        if (ret_f->tag == F_KEYWORD) {
            /* : int, : bool, etc. */
            const Symbol *kw = ret_f->as.sym;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                return_kind = TY_INT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                return_kind = TY_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                return_kind = TY_NIL;
            } else {
                diag_emit(DIAG_ERROR, ret_f->span,
                          "fn: unsupported return type keyword :%s",
                          kw->name);
                free(params);
                return NULL;
            }
            body_start = 3;
        }
    }

    /* Elaborate body */
    if (call->as.list.len < body_start + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "fn: missing body");
        free(params);
        return NULL;
    }

    /* Push a new scope for the function body with params bound */
    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    for (uint8_t i = 0; i < n_params; i++) {
        scope_add(&inner, params[i]);
    }

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;
    if (n_body == 1) {
        body = elab_form(e, call->as.list.items[body_start]);
        if (!body) { e->scope = inner.parent; scope_free(&inner); free(params); return NULL; }
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body; i++) {
            items[i] = elab_form(e, call->as.list.items[body_start + i]);
            if (!items[i]) { e->scope = inner.parent; scope_free(&inner); free(params); return NULL; }
        }
        body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n_body;
    }

    /* Pop scope */
    e->scope = inner.parent;
    scope_free(&inner);

    /* Create function type */
    TypeKind arg_kinds[MAX_FN_ARITY];
    for (uint8_t i = 0; i < n_params; i++) {
        arg_kinds[i] = TY_INT;  /* All int for phase 2 */
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);

    /* For anonymous fn, we lift it to a static function with a generated name.
     * We use the arena to allocate a unique name. */
    char fn_name_buf[32];
    snprintf(fn_name_buf, sizeof(fn_name_buf), "__fn_%u", e->next_id++);
    const Symbol *fn_name_sym = symtab_intern(e->st, 
        strslice(fn_name_buf, (uint32_t)strlen(fn_name_buf)));
    
    Binding *b = binding_new(e, fn_name_sym, fn_type, false, true, call->span);
    scope_add(&e->global, b);

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    fd->binding = b;
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = false;
    /* Store param types for codegen */
    fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint8_t i = 0; i < n_params; i++) {
        fd->param_types[i] = TYPE_INT;
    }

    /* Create the FN_DEF expression that will be emitted at file scope */
    Expr *fn_def_expr = expr_new(e->arena, EX_FN_DEF, fn_type, call->span);
    fn_def_expr->as.fn_def_.fn = fd;
    
    /* For Phase 2: if fn is at top level, we need to emit the function definition.
     * We do this by returning a special wrapper that contains both the FN_DEF
     * and a VAR reference. But for now, let's just emit the FN_DEF and return
     * a VAR reference. The emit_program will need to handle this.
     * 
     * Actually, for Phase 2, let's just return the FN_DEF directly and handle
     * it in emit. For nested fn (in let bindings), we'll need a different approach.
     */
    
    /* Return the FN_DEF expression which will be emitted at file scope */
    return fn_def_expr;
}

/* Phase 2: extern-c — (extern-c name [param1 param2 ...] : return-type)
 * Declares an external C function. For phase 2, we don't support capture.
 * Example: (extern-c printf [^cstr fmt] : int)
 * The ^ prefix on a param indicates it's a C type annotation (not yet implemented).
 * For now, all params are treated as int64_t or pointers.
 * 
 * Supported annotations:
 *   ^cstr - const char* (string)
 *   ^ptr  - void* (pointer)
 */
static Expr *elab_extern_c(Elab *e, const Form *call) {
    /* Minimum: (extern-c name [params...] : ret-type) */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "extern-c requires (extern-c name [params...] : ret-type)");
        return NULL;
    }

    /* Parse name */
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "extern-c name must be a symbol");
        return NULL;
    }
    if (scope_lookup(e->scope, name_f->as.sym)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "extern-c: '%s' is already defined", name_f->as.sym->name);
        return NULL;
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[2];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "extern-c: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* Parse params - for Phase 2, all params are int64_t
     * Type prefixes like ^cstr are not yet supported in parameter names */
    Binding **params = NULL;
    uint8_t n_params = 0;
    TypeKind param_kinds[MAX_FN_ARITY];

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "extern-c: parameter must be a symbol");
            return NULL;
        }
        if (n_params >= MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, p->span,
                      "extern-c: too many parameters (max %d)", MAX_FN_ARITY);
            return NULL;
        }
        
        /* For Phase 2, all extern-c params are treated as int64_t */
        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation */
    Form *ret_f = call->as.list.items[3];
    if (ret_f->tag != F_KEYWORD) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "extern-c: return type must be a keyword (:int, :bool, :void, :cstr, :ptr)");
        return NULL;
    }

    TypeKind return_kind = TY_NIL;
    const Symbol *kw = ret_f->as.sym;
    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
        return_kind = TY_INT;
    } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
        return_kind = TY_BOOL;
    } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
        return_kind = TY_NIL;
    } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
        return_kind = TY_CSTR;
    } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
        return_kind = TY_PTR_VOID;
    } else {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "extern-c: unsupported return type :%s", kw->name);
        return NULL;
    }

    /* Create function type */
    Type fn_type = type_fn(param_kinds, n_params, return_kind);

    /* Create a binding for the extern-c function so it can be looked up and called */
    Binding *b = binding_new(e, name_f->as.sym, fn_type, false, true, call->span);
    scope_add(&e->global, b);

    /* Create ExternC declaration */
    ExternC *ec = (ExternC *)arena_alloc(e->arena, sizeof(ExternC));
    ec->c_name = name_f->as.sym;
    ec->binding = b;
    ec->return_type = type_from_kind(return_kind);
    ec->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint8_t i = 0; i < n_params; i++) {
        ec->param_types[i] = type_from_kind(param_kinds[i]);
    }
    ec->n_params = n_params;
    ec->is_variadic = false;

    Expr *out = expr_new(e->arena, EX_EXTERN_C, fn_type, call->span);
    out->as.extern_c_.ext = ec;
    
    /* params was allocated with arena_alloc, so no need to free */
    return out;
}

static Expr *elab_def(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "def takes (def name init)");
        return NULL;
    }
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "def name must be a symbol");
        return NULL;
    }
    /* Top-level only — error if not in global scope. */
    if (e->scope != &e->global) {
        diag_emit(DIAG_ERROR, call->span, "def is only valid at the top level");
        return NULL;
    }
    if (scope_lookup(e->scope, name_f->as.sym)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "def: '%s' is already defined", name_f->as.sym->name);
        return NULL;
    }
    Expr *init = elab_form(e, call->as.list.items[2]);
    if (!init) return NULL;

    Binding *b = binding_new(e, name_f->as.sym, init->type,
                             /*is_mut=*/false, /*is_global=*/true, name_f->span);
    scope_add(&e->global, b);

    Expr *out = expr_new(e->arena, EX_DEF, TYPE_NIL, call->span);
    out->as.def_.binding = b;
    out->as.def_.init = init;
    return out;
}

/* ---- Phase 2: defn, fn, extern-c ---- */

static Expr *elab_defn(Elab *e, const Form *call);
static Expr *elab_fn(Elab *e, const Form *call);
static Expr *elab_extern_c(Elab *e, const Form *call);
static Expr *elab_call_fn(Elab *e, const Form *call, Binding *fn_binding);

/* ---- general elab ---- */

static Expr *elab_call(Elab *e, Form *call) {
    /* Already established: call->tag == F_LIST and len >= 1. */
    Form *head = call->as.list.items[0];
    if (head->tag != F_SYM) {
        diag_emit(DIAG_ERROR, head->span,
                  "call head must be a symbol (functions arrive in phase 2)");
        return NULL;
    }
    const Symbol *name = head->as.sym;

    /* Special forms. */
    if (name == e->sym_def)    return elab_def   (e, call);
    if (name == e->sym_let)    return elab_let   (e, call);
    if (name == e->sym_if)     return elab_if    (e, call);
    if (name == e->sym_do)     return elab_do    (e, call);
    if (name == e->sym_when)   return elab_when_unless(e, call, false);
    if (name == e->sym_unless) return elab_when_unless(e, call, true);
    if (name == e->sym_cond)   return elab_cond  (e, call);
    if (name == e->sym_set)    return elab_set   (e, call);
    if (name == e->sym_while)  return elab_while (e, call);
    /* Phase 2 */
    if (name == e->sym_defn)    return elab_defn  (e, call);
    if (name == e->sym_fn)      return elab_fn    (e, call);
    if (name == e->sym_extern_c) return elab_extern_c(e, call);

    /* Phase 2: Check if it's a user-defined function call */
    Binding *fn_binding = scope_lookup(e->scope, name);
    if (fn_binding && fn_binding->type.kind == TY_FN) {
        return elab_call_fn(e, call, fn_binding);
    }

    /* Builtin operator. Evaluate args first, then look up. */
    uint32_t n_args = call->as.list.len - 1;
    Expr **args = (n_args == 0) ? NULL :
        (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) return NULL;
    }
    Type first_t = (n_args > 0) ? args[0]->type : TYPE_NIL;
    const BuiltinSpec *spec = builtin_lookup(name, first_t, n_args);
    if (!spec) {
        const BuiltinSpec *any = builtin_first_with_name(name);
        if (any) {
            diag_emit(DIAG_ERROR, call->span,
                      "no matching overload for '%s' with %u arg(s) of type %s",
                      name->name, n_args,
                      n_args > 0 ? type_name(first_t) : "<none>");
        } else {
            diag_emit(DIAG_ERROR, head->span,
                      "unknown function or operator '%s'", name->name);
        }
        return NULL;
    }
    /* All args must match the spec's arg type. */
    for (uint32_t i = 0; i < n_args; i++) {
        if (!type_eq(args[i]->type, spec->arg_type)) {
            diag_emit(DIAG_ERROR, args[i]->span,
                      "'%s' arg %u: expected %s, got %s",
                      name->name, i + 1,
                      type_name(spec->arg_type), type_name(args[i]->type));
            return NULL;
        }
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    out->as.builtin.args = args;
    out->as.builtin.n = n_args;
    return out;
}

/* Phase 2: Elaborate a function call (f a b c) */
static Expr *elab_call_fn(Elab *e, const Form *call, Binding *fn_binding) {
    uint32_t n_args = call->as.list.len - 1;
    if (n_args == 0) {
        diag_emit(DIAG_ERROR, call->span, "function call requires at least one argument");
        return NULL;
    }

    /* Get the function type */
    Type fn_type = fn_binding->type;
    if (fn_type.kind != TY_FN) {
        diag_emit(DIAG_ERROR, call->span,
                  "'%s' is not a function", fn_binding->name->name);
        return NULL;
    }

    uint8_t expected_arity = fn_type.as.fn.arity;
    if (n_args != expected_arity) {
        diag_emit(DIAG_ERROR, call->span,
                  "function '%s' expects %u argument(s), got %u",
                  fn_binding->name->name, expected_arity, n_args);
        return NULL;
    }

    /* Elaborate arguments */
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) return NULL;
        /* For phase 2, all params are int, so expect int args */
        if (args[i]->type.kind != TY_INT) {
            diag_emit(DIAG_ERROR, args[i]->span,
                      "function '%s' arg %u: expected int, got %s",
                      fn_binding->name->name, i + 1, type_name(args[i]->type));
            return NULL;
        }
    }

    /* Result type is the function's return type */
    TypeKind result_kind = fn_type.as.fn.result_kind;
    Type result_type = type_from_kind(result_kind);

    Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
    out->as.call_.fn_binding = fn_binding;
    out->as.call_.args = args;
    out->as.call_.n_args = n_args;
    return out;
}

static Expr *elab_form(Elab *e, Form *f) {
    switch (f->tag) {
        case F_NIL:  return e_nil(e, f->span);
        case F_BOOL: {
            Expr *out = expr_new(e->arena, EX_BOOL_LIT, TYPE_BOOL, f->span);
            out->as.b = f->as.b;
            return out;
        }
        case F_INT: {
            Expr *out = expr_new(e->arena, EX_INT_LIT, TYPE_INT, f->span);
            out->as.i = f->as.i;
            return out;
        }
        case F_STR: {
            Expr *out = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, f->span);
            out->as.s = f->as.s;
            return out;
        }
        case F_KEYWORD:
            diag_emit(DIAG_ERROR, f->span,
                      "phase 1: keywords are only allowed as :else in cond");
            return NULL;
        case F_SYM: {
            Binding *b = scope_lookup(e->scope, f->as.sym);
            if (!b) {
                diag_emit(DIAG_ERROR, f->span,
                          "unbound symbol '%s'", f->as.sym->name);
                return NULL;
            }
            Expr *out = expr_new(e->arena, EX_VAR, b->type, f->span);
            out->as.var.binding = b;
            return out;
        }
        case F_VEC:
            diag_emit(DIAG_ERROR, f->span,
                      "phase 1: vector literals are only allowed in let bindings");
            return NULL;
        case F_CBLOCK: {
            /* Phase 2: inline C code block ```c ... ``` */
            /* For now, we don't support captures, so the InlineC has no captures */
            InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
            ic->code = f->as.cblock;
            ic->return_type = TYPE_NIL; /* Will be inferred from context or default to void */
            ic->captures = NULL;
            ic->n_captures = 0;
            
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
    }
    return NULL;
}

Expr *elaborate_program(Arena *arena, SymbolTable *st,
                        Form *const *forms, uint32_t nforms) {
    Elab e;
    elab_init_state(&e, arena, st);
    builtins_init(st);

    Expr **items = (nforms == 0) ? NULL :
        (Expr **)arena_alloc(arena, nforms * sizeof(Expr *));
    int rc = 0;

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
                        Form *name_f = f->as.list.items[1];
                        if (name_f->tag == F_SYM) {
                            /* Parse return type annotation if present */
                            TypeKind return_kind = TY_INT; /* default */
                            if (f->as.list.len >= 4) {
                                Form *ret_f = f->as.list.items[3];
                                if (ret_f->tag == F_KEYWORD) {
                                    const Symbol *kw = ret_f->as.sym;
                                    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                                        return_kind = TY_INT;
                                    } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                                        return_kind = TY_BOOL;
                                    } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                                        return_kind = TY_NIL;
                                    } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                                        return_kind = TY_CSTR;
                                    } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                                        return_kind = TY_PTR_VOID;
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
                }
            }
        }
    }

    /* Pass 2: Elaborate all forms */
    for (uint32_t i = 0; i < nforms; i++) {
        items[i] = elab_form(&e, forms[i]);
        if (!items[i]) { rc = -1; /* keep going to surface more diagnostics */ }
    }

    scope_free(&e.global);
    if (rc != 0) return NULL;

    Expr *prog = expr_new(arena, EX_PROGRAM, TYPE_NIL,
                          nforms > 0 ? forms[0]->span : (Span){0,0,0,0,0,0});
    prog->as.program.items = items;
    prog->as.program.n = nforms;
    return prog;
}
