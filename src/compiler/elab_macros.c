/* elab_macros.c -- compile-time evaluation, quasiquote, macro definition and expansion. */
#include "elab_internal.h"

/* ---- file-local helper forward declarations ---- */
static CtValue ct_value_form(Form *f);
static CtValue ct_value_fn(CtFn *fn);
static CtEnv *ct_env_new(Elab *e, CtEnv *parent, bool *ok);
static void ct_env_bind(CtEnv *env, const Symbol *name, CtValue v);
static bool ct_env_lookup(CtEnv *env, const Symbol *name, CtValue *out);
static bool compile_time_truthy(Form *f);
static bool ct_form_equal(const Form *a, const Form *b);
static Form *ct_value_to_form(CtEnv *env, CtValue v, Span span);
static Form *ct_eval_quasiquote(CtEnv *env, Form *f);
static bool ct_symbol_name(const Symbol *sym, const char *name);
static bool form_contains_ct_builtins(Form *f);
static CtValue ct_eval_builtin(CtEnv *env, const Symbol *name, Form **args, uint32_t n_args, Span span);
static CtValue ct_eval_call(CtEnv *env, Form *f);
static CtValue ct_eval_form(CtEnv *env, Form *f);
static Form *elab_eval_macro_form(Elab *e, Form *f);
static Form *substitute_params(Elab *e, Form *f, MacroDef *macro, Form **args);

static CtValue ct_value_form(Form *f) {
    CtValue v;
    v.tag = CT_VAL_FORM;
    v.as.form = f;
    return v;
}

static CtValue ct_value_fn(CtFn *fn) {
    CtValue v;
    v.tag = CT_VAL_FN;
    v.as.fn = fn;
    return v;
}

static CtEnv *ct_env_new(Elab *e, CtEnv *parent, bool *ok) {
    CtEnv *env = (CtEnv *)arena_alloc(e->arena, sizeof(CtEnv));
    env->parent = parent;
    env->bindings = NULL;
    env->elab = e;
    env->ok = ok;
    return env;
}

static void ct_env_bind(CtEnv *env, const Symbol *name, CtValue v) {
    CtBinding *b = (CtBinding *)arena_alloc(env->elab->arena, sizeof(CtBinding));
    b->name = name;
    b->value = v;
    b->next = env->bindings;
    env->bindings = b;
}

static bool ct_env_lookup(CtEnv *env, const Symbol *name, CtValue *out) {
    for (CtEnv *cur = env; cur; cur = cur->parent) {
        for (CtBinding *b = cur->bindings; b; b = b->next) {
            if (b->name == name) {
                *out = b->value;
                return true;
            }
        }
    }
    return false;
}

static bool compile_time_truthy(Form *f) {
    return !(f->tag == F_NIL || (f->tag == F_BOOL && !f->as.b));
}

static bool ct_form_equal(const Form *a, const Form *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->tag != b->tag) return false;
    switch (a->tag) {
        case F_NIL: return true;
        case F_BOOL: return a->as.b == b->as.b;
        case F_INT: return a->as.i == b->as.i;
        case F_FLOAT: return a->as.f == b->as.f;
        case F_STR:
            return a->as.s.len == b->as.s.len &&
                   strncmp(a->as.s.p, b->as.s.p, a->as.s.len) == 0;
        case F_SYM:
        case F_KEYWORD:
            return a->as.sym == b->as.sym;
        case F_QUOTE:
        case F_QUASIQUOTE:
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
        case F_TYPE_ANN:
        /* CT0: Contract types compare structurally */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals compare structurally */
        case F_READER_COND:
        /* RR3: Range literal variable annotation compares structurally */
        case F_RANGE_VAR:
        case F_LIST:
        case F_VEC:
        case F_MAP:
        case F_SET:
            if (a->as.list.len != b->as.list.len) return false;
            for (uint32_t i = 0; i < a->as.list.len; i++) {
                if (!ct_form_equal(a->as.list.items[i], b->as.list.items[i])) return false;
            }
            return true;
        case F_CBLOCK:
            return a->as.cblock.len == b->as.cblock.len &&
                   strncmp(a->as.cblock.p, b->as.cblock.p, a->as.cblock.len) == 0;
    }
    return false;
}

static Form *ct_value_to_form(CtEnv *env, CtValue v, Span span) {
    if (v.tag == CT_VAL_FORM) return v.as.form;
    *env->ok = false;
    diag_emit(DIAG_ERROR, span, "compile-time macro evaluation expected a form value");
    return form_nil(env->elab->arena, span);
}

static Form *ct_eval_quasiquote(CtEnv *env, Form *f) {
    switch (f->tag) {
        case F_UNQUOTE: {
            CtValue v = ct_eval_form(env, f->as.list.items[0]);
            return ct_value_to_form(env, v, f->span);
        }
        case F_UNQUOTE_SPLICING: {
            CtValue v = ct_eval_form(env, f->as.list.items[0]);
            return ct_value_to_form(env, v, f->span);
        }
        case F_LIST: {
            uint32_t n_in = f->as.list.len;
            Form **tmp = (Form **)arena_alloc(env->elab->arena, n_in * sizeof(Form *));
            bool *splice = (bool *)arena_alloc(env->elab->arena, n_in * sizeof(bool));
            uint32_t n_out = 0;
            for (uint32_t i = 0; i < n_in; i++) {
                Form *it = f->as.list.items[i];
                if (it->tag == F_UNQUOTE_SPLICING) {
                    CtValue v = ct_eval_form(env, it->as.list.items[0]);
                    Form *inner = ct_value_to_form(env, v, it->span);
                    if (!*env->ok) return form_nil(env->elab->arena, f->span);
                    tmp[i] = inner;
                    if (inner->tag == F_LIST) {
                        splice[i] = true;
                        n_out += inner->as.list.len;
                    } else {
                        splice[i] = false;
                        n_out++;
                    }
                } else {
                    tmp[i] = ct_eval_quasiquote(env, it);
                    splice[i] = false;
                    n_out++;
                }
            }
            Form **items = (Form **)arena_alloc(env->elab->arena, n_out * sizeof(Form *));
            uint32_t out_idx = 0;
            for (uint32_t i = 0; i < n_in; i++) {
                if (splice[i]) {
                    Form *lst = tmp[i];
                    for (uint32_t j = 0; j < lst->as.list.len; j++) items[out_idx++] = lst->as.list.items[j];
                } else {
                    items[out_idx++] = tmp[i];
                }
            }
            return form_list(env->elab->arena, f->span, items, n_out);
        }
        case F_VEC:
        case F_MAP:
        case F_SET: {
            uint32_t n_in = f->as.list.len;
            Form **items = (Form **)arena_alloc(env->elab->arena, n_in * sizeof(Form *));
            for (uint32_t i = 0; i < n_in; i++) items[i] = ct_eval_quasiquote(env, f->as.list.items[i]);
            if (f->tag == F_MAP) return form_map(env->elab->arena, f->span, items, n_in);
            if (f->tag == F_SET) return form_set(env->elab->arena, f->span, items, n_in);
            return form_vec(env->elab->arena, f->span, items, n_in);
        }
        case F_QUASIQUOTE:
            return ct_eval_quasiquote(env, f->as.list.items[0]);
        default:
            return f;
    }
}

static bool ct_symbol_name(const Symbol *sym, const char *name) {
    size_t n = strlen(name);
    return sym->len == n && strncmp(sym->name, name, n) == 0;
}

static bool form_contains_ct_builtins(Form *f) {
    switch (f->tag) {
        case F_LIST:
            if (f->as.list.len > 0 && f->as.list.items[0]->tag == F_SYM) {
                const Symbol *head = f->as.list.items[0]->as.sym;
                if (ct_symbol_name(head, "first") ||
                    ct_symbol_name(head, "rest") ||
                    ct_symbol_name(head, "second") ||
                    ct_symbol_name(head, "nil?") ||
                    ct_symbol_name(head, "empty?") ||
                    ct_symbol_name(head, "list?") ||
                    ct_symbol_name(head, "vec?") ||
                    ct_symbol_name(head, "symbol-name") ||
                    ct_symbol_name(head, "dot-sym") ||
                    ct_symbol_name(head, "str-append") ||
                    ct_symbol_name(head, "cons") ||
                    ct_symbol_name(head, "list") ||
                    ct_symbol_name(head, "vec")) {
                    return true;
                }
            }
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (form_contains_ct_builtins(f->as.list.items[i])) return true;
            }
            return false;
        case F_VEC:
        case F_MAP:
        case F_SET:
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (form_contains_ct_builtins(f->as.list.items[i])) return true;
            }
            return false;
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            return form_contains_ct_builtins(f->as.list.items[0]);
        case F_QUOTE:
        case F_QUASIQUOTE:
        case F_SYM:
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_CBLOCK:
        case F_TYPE_ANN:
        /* CT0: Contract type annotations don't contain CT builtins at top level */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals don't contain CT builtins */
        case F_READER_COND:
        /* RR3: Range literal variable annotation doesn't contain CT builtins */
        case F_RANGE_VAR:
            return false;
    }
    return false;
}

static CtValue ct_eval_builtin(CtEnv *env, const Symbol *name, Form **args, uint32_t n_args, Span span) {
    if (ct_symbol_name(name, "first")) {
        if (n_args != 1) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, span, "compile-time first expects 1 argument");
            return ct_value_form(form_nil(env->elab->arena, span));
        }
        if ((args[0]->tag != F_LIST && args[0]->tag != F_VEC) || args[0]->as.list.len == 0) return ct_value_form(form_nil(env->elab->arena, span));
        return ct_value_form(args[0]->as.list.items[0]);
    }
    if (ct_symbol_name(name, "rest")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time rest expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        if ((args[0]->tag != F_LIST && args[0]->tag != F_VEC) || args[0]->as.list.len <= 1) return ct_value_form(form_list(env->elab->arena, span, NULL, 0));
        uint32_t len = args[0]->as.list.len - 1;
        Form **items = (Form **)arena_alloc(env->elab->arena, len * sizeof(Form *));
        for (uint32_t i = 0; i < len; i++) items[i] = args[0]->as.list.items[i + 1];
        return ct_value_form(form_list(env->elab->arena, span, items, len));
    }
    if (ct_symbol_name(name, "second")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time second expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        if ((args[0]->tag != F_LIST && args[0]->tag != F_VEC) || args[0]->as.list.len < 2) return ct_value_form(form_nil(env->elab->arena, span));
        return ct_value_form(args[0]->as.list.items[1]);
    }
    if (ct_symbol_name(name, "nil?")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time nil? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, args[0]->tag == F_NIL));
    }
    if (ct_symbol_name(name, "empty?")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time empty? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        bool empty = args[0]->tag == F_NIL ||
                     ((args[0]->tag == F_LIST || args[0]->tag == F_VEC || args[0]->tag == F_MAP || args[0]->tag == F_SET) && args[0]->as.list.len == 0);
        return ct_value_form(form_bool(env->elab->arena, span, empty));
    }
    if (ct_symbol_name(name, "list?")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time list? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, args[0]->tag == F_LIST));
    }
    if (ct_symbol_name(name, "vec?")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time vec? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, args[0]->tag == F_VEC));
    }
    if (ct_symbol_name(name, "symbol-name")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time symbol-name expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        if (args[0]->tag != F_SYM) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time symbol-name expects a symbol"); return ct_value_form(form_nil(env->elab->arena, span)); }
        const Symbol *sym = args[0]->as.sym;
        return ct_value_form(form_str(env->elab->arena, span, sym->name, sym->len));
    }
    if (ct_symbol_name(name, "dot-sym")) {
        /* (dot-sym field-sym) => the symbol ".field-sym" for use in method calls */
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time dot-sym expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        if (args[0]->tag != F_SYM) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time dot-sym expects a symbol"); return ct_value_form(form_nil(env->elab->arena, span)); }
        const Symbol *sym = args[0]->as.sym;
        uint32_t new_len = sym->len + 1;
        char *buf = (char *)arena_alloc(env->elab->arena, new_len + 1);
        buf[0] = '.';
        memcpy(buf + 1, sym->name, sym->len);
        buf[new_len] = '\0';
        const Symbol *new_sym = symtab_intern(env->elab->st, strslice(buf, new_len));
        return ct_value_form(form_sym(env->elab->arena, span, new_sym));
    }
    if (ct_symbol_name(name, "str-append")) {
        /* (str-append s1 s2 ...) => compile-time string concatenation */
        size_t total = 0;
        for (uint32_t i = 0; i < n_args; i++) {
            if (args[i]->tag != F_STR) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time str-append expects string arguments"); return ct_value_form(form_nil(env->elab->arena, span)); }
            total += args[i]->as.s.len;
        }
        char *buf = (char *)arena_alloc(env->elab->arena, total + 1);
        size_t off = 0;
        for (uint32_t i = 0; i < n_args; i++) {
            memcpy(buf + off, args[i]->as.s.p, args[i]->as.s.len);
            off += args[i]->as.s.len;
        }
        buf[total] = '\0';
        return ct_value_form(form_str(env->elab->arena, span, buf, (uint32_t)total));
    }
    if (ct_symbol_name(name, "cons")) {
        if (n_args != 2) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time cons expects 2 arguments"); return ct_value_form(form_nil(env->elab->arena, span)); }
        Form *tail = args[1];
        uint32_t tail_len = 0;
        if (tail->tag == F_LIST) {
            tail_len = tail->as.list.len;
        } else if (tail->tag != F_NIL) {
            tail_len = 1;
        }
        Form **items = (Form **)arena_alloc(env->elab->arena, (tail_len + 1) * sizeof(Form *));
        items[0] = args[0];
        if (tail->tag == F_LIST) {
            for (uint32_t i = 0; i < tail_len; i++) items[i + 1] = tail->as.list.items[i];
        } else if (tail->tag != F_NIL) {
            items[1] = tail;
        }
        return ct_value_form(form_list(env->elab->arena, span, items, tail_len + 1));
    }
    if (ct_symbol_name(name, "list")) {
        Form **items = (n_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_args * sizeof(Form *));
        for (uint32_t i = 0; i < n_args; i++) items[i] = args[i];
        return ct_value_form(form_list(env->elab->arena, span, items, n_args));
    }
    if (ct_symbol_name(name, "vec")) {
        Form **items = (n_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_args * sizeof(Form *));
        for (uint32_t i = 0; i < n_args; i++) items[i] = args[i];
        return ct_value_form(form_vec(env->elab->arena, span, items, n_args));
    }
    if (ct_symbol_name(name, "=")) {
        if (n_args != 2) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time = expects 2 arguments"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, ct_form_equal(args[0], args[1])));
    }
    if (ct_symbol_name(name, "not")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time not expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, !compile_time_truthy(args[0])));
    }
    return ct_value_form(NULL);
}

static CtValue ct_eval_call(CtEnv *env, Form *f) {
    if (f->as.list.len == 0) return ct_value_form(form_list(env->elab->arena, f->span, NULL, 0));
    Form *head = f->as.list.items[0];
    /* Non-symbol head: cannot be a CT special form or builtin.
     * Return the form as-is so it can be elaborated later.
     * This preserves unevaluated clause lists inside variadic macros. */
    if (head->tag != F_SYM) {
        return ct_value_form(f);
    }
    /* At this point head->tag == F_SYM is guaranteed by the early return above. */
    if (head->as.sym == env->elab->sym_quote && f->as.list.len == 2) {
        return ct_value_form(f->as.list.items[1]);
    }
    if (head->as.sym == env->elab->sym_quasiquote && f->as.list.len == 2) {
        return ct_value_form(ct_eval_quasiquote(env, f->as.list.items[1]));
    }
    if (head->as.sym == env->elab->sym_if) {
        if (f->as.list.len < 3 || f->as.list.len > 4) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time if requires 2 or 3 arguments");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        CtValue cond_v = ct_eval_form(env, f->as.list.items[1]);
        Form *cond = ct_value_to_form(env, cond_v, f->as.list.items[1]->span);
        if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
        if (cond->tag == F_BOOL || cond->tag == F_NIL) {
            if (compile_time_truthy(cond)) return ct_eval_form(env, f->as.list.items[2]);
            if (f->as.list.len == 4) return ct_eval_form(env, f->as.list.items[3]);
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        Form **items = (Form **)arena_alloc(env->elab->arena, f->as.list.len * sizeof(Form *));
        items[0] = head;
        items[1] = cond;
        CtValue then_v = ct_eval_form(env, f->as.list.items[2]);
        items[2] = ct_value_to_form(env, then_v, f->as.list.items[2]->span);
        if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
        if (f->as.list.len == 4) {
            CtValue else_v = ct_eval_form(env, f->as.list.items[3]);
            items[3] = ct_value_to_form(env, else_v, f->as.list.items[3]->span);
            if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        return ct_value_form(form_list(env->elab->arena, f->span, items, f->as.list.len));
    }
    if (head->as.sym == env->elab->sym_do) {
        CtValue out = ct_value_form(form_nil(env->elab->arena, f->span));
        for (uint32_t i = 1; i < f->as.list.len; i++) {
            out = ct_eval_form(env, f->as.list.items[i]);
            if (!*env->ok) return out;
        }
        return out;
    }
    if (head->as.sym == env->elab->sym_let) {
        if (f->as.list.len < 3) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time let requires bindings and body");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        Form *bindings = f->as.list.items[1];
        if (bindings->tag != F_VEC && bindings->tag != F_LIST) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, bindings->span, "compile-time let bindings must be a vector or list");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        if ((bindings->as.list.len % 2) != 0) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, bindings->span, "compile-time let requires an even number of binding forms");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        CtEnv *inner = ct_env_new(env->elab, env, env->ok);
        for (uint32_t i = 0; i < bindings->as.list.len; i += 2) {
            Form *name_f = bindings->as.list.items[i];
            if (name_f->tag != F_SYM) {
                *env->ok = false;
                diag_emit(DIAG_ERROR, name_f->span, "compile-time let binding name must be a symbol");
                return ct_value_form(form_nil(env->elab->arena, f->span));
            }
            CtValue init_v = ct_eval_form(inner, bindings->as.list.items[i + 1]);
            if (!*env->ok) return init_v;
            ct_env_bind(inner, name_f->as.sym, init_v);
        }
        CtValue out = ct_value_form(form_nil(env->elab->arena, f->span));
        for (uint32_t i = 2; i < f->as.list.len; i++) {
            out = ct_eval_form(inner, f->as.list.items[i]);
            if (!*env->ok) return out;
        }
        return out;
    }
    if (head->as.sym == env->elab->sym_fn) {
        if (f->as.list.len < 3) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time fn requires params and body");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        Form *params_f = f->as.list.items[1];
        if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, params_f->span, "compile-time fn params must be a vector or list");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        uint32_t n_total = params_f->as.list.len;
        Form **params = (Form **)arena_alloc(env->elab->arena, n_total * sizeof(Form *));
        uint32_t n_fixed = 0;
        bool is_variadic = false;
        const Symbol *rest_param = NULL;
        for (uint32_t i = 0; i < n_total; i++) {
            Form *p = params_f->as.list.items[i];
            if (p->tag != F_SYM) {
                *env->ok = false;
                diag_emit(DIAG_ERROR, p->span, "compile-time fn parameter must be a symbol");
                return ct_value_form(form_nil(env->elab->arena, f->span));
            }
            if (p->as.sym == env->elab->sym_borrow) {
                if (i + 1 >= n_total) {
                    *env->ok = false;
                    diag_emit(DIAG_ERROR, p->span, "compile-time fn '&' must be followed by a rest parameter");
                    return ct_value_form(form_nil(env->elab->arena, f->span));
                }
                if (i + 2 < n_total) {
                    *env->ok = false;
                    diag_emit(DIAG_ERROR, params_f->as.list.items[i + 2]->span, "compile-time fn allows only one rest parameter");
                    return ct_value_form(form_nil(env->elab->arena, f->span));
                }
                Form *rest_f = params_f->as.list.items[i + 1];
                if (rest_f->tag != F_SYM) {
                    *env->ok = false;
                    diag_emit(DIAG_ERROR, rest_f->span, "compile-time fn rest parameter must be a symbol");
                    return ct_value_form(form_nil(env->elab->arena, f->span));
                }
                is_variadic = true;
                rest_param = rest_f->as.sym;
                break;
            }
            params[n_fixed++] = p;
        }
        Form *body = NULL;
        if (f->as.list.len == 3) {
            body = f->as.list.items[2];
        } else {
            uint32_t n_body = f->as.list.len - 2;
            Form **body_items = (Form **)arena_alloc(env->elab->arena, (n_body + 1) * sizeof(Form *));
            body_items[0] = form_sym(env->elab->arena, f->span, env->elab->sym_do);
            for (uint32_t i = 0; i < n_body; i++) body_items[i + 1] = f->as.list.items[i + 2];
            body = form_list(env->elab->arena, f->span, body_items, n_body + 1);
        }
        CtFn *fn = (CtFn *)arena_alloc(env->elab->arena, sizeof(CtFn));
        fn->params = params;
        fn->n_params = n_fixed;
        fn->is_variadic = is_variadic;
        fn->rest_param = rest_param;
        fn->body = body;
        fn->env = env;
        fn->span = f->span;
        return ct_value_fn(fn);
    }

    CtValue head_v = ct_eval_form(env, head);
    if (!*env->ok) return head_v;
    uint32_t n_in_args = f->as.list.len - 1;
    Form **tmp_args = (n_in_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_in_args * sizeof(Form *));
    bool *splice = (n_in_args == 0) ? NULL : (bool *)arena_alloc(env->elab->arena, n_in_args * sizeof(bool));
    uint32_t n_args = 0;
    for (uint32_t i = 0; i < n_in_args; i++) {
        Form *arg_form = f->as.list.items[i + 1];
        if (arg_form->tag == F_UNQUOTE_SPLICING) {
            CtValue v = ct_eval_form(env, arg_form->as.list.items[0]);
            Form *inner = ct_value_to_form(env, v, arg_form->span);
            if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            tmp_args[i] = inner;
            if (inner->tag == F_LIST) {
                splice[i] = true;
                n_args += inner->as.list.len;
            } else {
                splice[i] = false;
                n_args += 1;
            }
        } else {
            CtValue v = ct_eval_form(env, arg_form);
            Form *evaluated = ct_value_to_form(env, v, arg_form->span);
            if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            tmp_args[i] = evaluated;
            splice[i] = false;
            n_args += 1;
        }
    }
    Form **args = (n_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_args * sizeof(Form *));
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < n_in_args; i++) {
        if (splice && splice[i]) {
            Form *lst = tmp_args[i];
            for (uint32_t j = 0; j < lst->as.list.len; j++) args[out_idx++] = lst->as.list.items[j];
        } else if (n_args > 0) {
            args[out_idx++] = tmp_args ? tmp_args[i] : NULL;
        }
    }

    /* head->tag == F_SYM is guaranteed here by the early return above. */
    {
        CtValue b = ct_eval_builtin(env, head->as.sym, args, n_args, f->span);
        if (b.tag == CT_VAL_FORM && b.as.form != NULL) return b;
    }

    if (head_v.tag == CT_VAL_FN) {
        CtFn *fn = head_v.as.fn;
        if (!fn->is_variadic && n_args != fn->n_params) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time fn expected %u arguments, got %u", fn->n_params, n_args);
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        if (fn->is_variadic && n_args < fn->n_params) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time fn expected at least %u arguments, got %u", fn->n_params, n_args);
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        CtEnv *call_env = ct_env_new(env->elab, fn->env, env->ok);
        for (uint32_t i = 0; i < fn->n_params; i++) ct_env_bind(call_env, fn->params[i]->as.sym, ct_value_form(args[i]));
        if (fn->is_variadic) {
            uint32_t n_rest = n_args - fn->n_params;
            Form **rest = (n_rest == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_rest * sizeof(Form *));
            for (uint32_t i = 0; i < n_rest; i++) rest[i] = args[fn->n_params + i];
            ct_env_bind(call_env, fn->rest_param, ct_value_form(form_list(env->elab->arena, f->span, rest, n_rest)));
        }
        return ct_eval_form(call_env, fn->body);
    }

    Form *head_form = ct_value_to_form(env, head_v, head->span);
    if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
    Form **items = (Form **)arena_alloc(env->elab->arena, (n_args + 1) * sizeof(Form *));
    items[0] = head_form;
    for (uint32_t i = 0; i < n_args; i++) items[i + 1] = args[i];
    return ct_value_form(form_list(env->elab->arena, f->span, items, n_args + 1));
}

static CtValue ct_eval_form(CtEnv *env, Form *f) {
    switch (f->tag) {
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_QUOTE:
        case F_CBLOCK:
            return ct_value_form(f);
        case F_QUASIQUOTE:
            return ct_value_form(ct_eval_quasiquote(env, f->as.list.items[0]));
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            return ct_value_form(f->as.list.items[0]);
        case F_TYPE_ANN:
        /* CT0: Contract type annotations are compile-time literals */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals are compile-time literals */
        case F_READER_COND:
        /* RR3: Range literal variable annotation is a compile-time literal */
        case F_RANGE_VAR:
            /* Type annotations are compile-time literals */
            return ct_value_form(f);
        case F_SYM: {
            CtValue v;
            if (ct_env_lookup(env, f->as.sym, &v)) return v;
            return ct_value_form(f);
        }
        case F_LIST:
            return ct_eval_call(env, f);
        case F_VEC:
        case F_MAP:
        case F_SET: {
            Form **items = (f->as.list.len == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                CtValue v = ct_eval_form(env, f->as.list.items[i]);
                items[i] = ct_value_to_form(env, v, f->as.list.items[i]->span);
                if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            }
            if (f->tag == F_MAP) return ct_value_form(form_map(env->elab->arena, f->span, items, f->as.list.len));
            if (f->tag == F_SET) return ct_value_form(form_set(env->elab->arena, f->span, items, f->as.list.len));
            return ct_value_form(form_vec(env->elab->arena, f->span, items, f->as.list.len));
        }
    }
    return ct_value_form(f);
}

static Form *elab_eval_macro_form(Elab *e, Form *f) {
    bool ok = true;
    CtEnv *env = ct_env_new(e, NULL, &ok);
    CtValue v = ct_eval_form(env, f);
    if (!ok) return NULL;
    if (v.tag != CT_VAL_FORM) {
        diag_emit(DIAG_ERROR, f->span, "compile-time macro body did not evaluate to a form");
        return NULL;
    }
    return v.as.form;
}

/* Phase 6: Register a macro */
void elab_register_macro(Elab *e, MacroDef *macro) {
    if (e->n_macros >= e->cap_macros) {
        e->cap_macros = e->cap_macros ? e->cap_macros * 2 : 8;
        e->macros = (MacroDef **)realloc(e->macros, e->cap_macros * sizeof(MacroDef *));
    }
    e->macros[e->n_macros++] = macro;
}

/* Phase 6: Expand quasiquote forms recursively */
Form *quasiquote_expand_form(Elab *e, Form *f) {
    switch (f->tag) {
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_SYM:
            /* Literals and symbols: (quasiquote x) -> (quote x) */
            return form_quote(e->arena, f->span, f);
        case F_QUOTE:
            /* (quasiquote (quote x)) -> (quote (quasiquote x)) */
            {
                Form *quoted_form = f->as.list.items[0];
                Form *expanded_quoted = quasiquote_expand_form(e, quoted_form);
                return form_quote(e->arena, f->span, expanded_quoted);
            }
        case F_UNQUOTE:
            /* (quasiquote (unquote x)) -> x */
            return f->as.list.items[0];
        case F_UNQUOTE_SPLICING:
            /* (quasiquote (unquote-splicing x)) -> x */
            return f->as.list.items[0];
        case F_LIST: {
            /* Check for gensym call: (gensym ...) */
            if (f->as.list.len >= 1 && f->as.list.items[0]->tag == F_SYM) {
                const Symbol *name = f->as.list.items[0]->as.sym;
                if (name == e->sym_gensym) {
                    /* Generate a fresh symbol for gensym inside quasiquote */
                    const char *prefix = "g";
                    if (f->as.list.len >= 2) {
                        Form *prefix_f = f->as.list.items[1];
                        if (prefix_f->tag == F_SYM) {
                            prefix = prefix_f->as.sym->name;
                        } else if (prefix_f->tag == F_STR) {
                            prefix = prefix_f->as.s.p;
                        }
                    }
                    char name_buf[64];
                    snprintf(name_buf, sizeof(name_buf), "%s_%u", prefix, e->next_gensym_id++);
                    const Symbol *fresh_sym = symtab_intern(e->st, strslice(name_buf, (uint32_t)strlen(name_buf)));
                    return form_sym(e->arena, f->span, fresh_sym);
                }
            }
            /* Lists inside quasiquote: process each element */
            /* (quasiquote (a ~b c)) -> (a b c) with b unbound from macro params */
            /* For phase 6, we build a list with expanded elements */
            {
                Form **new_items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
                for (uint32_t i = 0; i < f->as.list.len; i++) {
                    Form *item = f->as.list.items[i];
                    if (item->tag == F_UNQUOTE || item->tag == F_UNQUOTE_SPLICING) {
                        /* Unwrap unquote/unquote-splicing - just return the inner form */
                        /* Parameter substitution will happen in substitute_params */
                        new_items[i] = item->as.list.items[0];
                    } else {
                        /* Recursively expand other items */
                        new_items[i] = quasiquote_expand_form(e, item);
                    }
                }
                return form_list(e->arena, f->span, new_items, f->as.list.len);
            }
        }
        case F_VEC:
        case F_MAP:
        case F_SET:
            /* Vectors/maps/sets inside quasiquote: process each element */
            {
                Form **new_items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
                for (uint32_t i = 0; i < f->as.list.len; i++) {
                    Form *item = f->as.list.items[i];
                    if (item->tag == F_UNQUOTE || item->tag == F_UNQUOTE_SPLICING) {
                        new_items[i] = item->as.list.items[0];
                    } else {
                        new_items[i] = quasiquote_expand_form(e, item);
                    }
                }
                if (f->tag == F_MAP) return form_map(e->arena, f->span, new_items, f->as.list.len);
                if (f->tag == F_SET) return form_set(e->arena, f->span, new_items, f->as.list.len);
                return form_vec(e->arena, f->span, new_items, f->as.list.len);
            }
        case F_CBLOCK:
        case F_TYPE_ANN:
        /* CT0: Contract type annotations are passed through in quasiquote */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals are passed through in quasiquote */
        case F_READER_COND:
        /* RR3: Range literal variable annotation is passed through in quasiquote */
        case F_RANGE_VAR:
            return f;
        case F_QUASIQUOTE:
            /* Expand the quasiquoted form */
            return quasiquote_expand_form(e, f->as.list.items[0]);
    }
    return f;
}

/* Phase 6: Helper to substitute parameters in a form */
static Form *substitute_params(Elab *e, Form *f, MacroDef *macro, Form **args) {
    switch (f->tag) {
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_CBLOCK:
        case F_QUOTE:
        case F_TYPE_ANN:
        /* CT0: Contract type annotations are returned as-is in macro substitution */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals are returned as-is in macro substitution */
        case F_READER_COND:
        /* RR3: Range literal variable annotation is returned as-is in macro substitution */
        case F_RANGE_VAR:
            /* Literals, quote forms, and type annotations are returned as-is */
            return f;
        case F_QUASIQUOTE:
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            /* Expand quasiquote forms first, then continue substitution */
            return substitute_params(e, quasiquote_expand_form(e, f), macro, args);
        case F_LIST: {
            /* Check for gensym call: (gensym) or (gensym prefix) */
            if (f->as.list.len >= 1 && f->as.list.items[0]->tag == F_SYM) {
                const Symbol *name = f->as.list.items[0]->as.sym;
                if (name == e->sym_gensym) {
                    /* This is a gensym call - generate a fresh symbol */
                    const char *prefix = "g";
                    if (f->as.list.len >= 2) {
                        Form *prefix_f = f->as.list.items[1];
                        if (prefix_f->tag == F_SYM) {
                            prefix = prefix_f->as.sym->name;
                        } else if (prefix_f->tag == F_STR) {
                            prefix = prefix_f->as.s.p;
                        }
                        /* For other cases, use default prefix */
                    }
                    char name_buf[64];
                    snprintf(name_buf, sizeof(name_buf), "%s_%u", prefix, e->next_gensym_id++);
                    const Symbol *fresh_sym = symtab_intern(e->st, strslice(name_buf, (uint32_t)strlen(name_buf)));
                    return form_sym(e->arena, f->span, fresh_sym);
                }
            }
            /* Not a gensym call - continue with normal processing */
            /* Check if any item is ~@ (unquote-splicing) — use splice path if so */
            bool has_splice = false;
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (f->as.list.items[i]->tag == F_UNQUOTE_SPLICING) {
                    has_splice = true; break;
                }
            }
            if (has_splice) {
                /* Two-pass splice: substitute each item, splicing F_UNQUOTE_SPLICING */
                uint32_t n_in = f->as.list.len;
                Form **temp = (Form **)arena_alloc(e->arena, n_in * sizeof(Form *));
                bool *is_splice = (bool *)arena_alloc(e->arena, n_in * sizeof(bool));
                bool *keep_splice_wrapper = (bool *)arena_alloc(e->arena, n_in * sizeof(bool));
                uint32_t n_out = 0;
                for (uint32_t i = 0; i < n_in; i++) {
                    Form *item = f->as.list.items[i];
                    if (item->tag == F_UNQUOTE_SPLICING) {
                        /* ~@ inner: substitute the inner form, then splice if it's a list */
                        Form *inner = item->as.list.items[0];
                        Form *subst = substitute_params(e, inner, macro, args);
                        temp[i] = subst;
                        if (subst->tag == F_LIST) {
                            is_splice[i] = true;
                            keep_splice_wrapper[i] = false;
                            n_out += subst->as.list.len;
                        } else {
                            is_splice[i] = false;
                            keep_splice_wrapper[i] = true;
                            n_out++;
                        }
                    } else {
                        temp[i] = substitute_params(e, item, macro, args);
                        is_splice[i] = false;
                        keep_splice_wrapper[i] = false;
                        n_out++;
                    }
                }
                Form **items = (Form **)arena_alloc(e->arena, n_out * sizeof(Form *));
                uint32_t out_idx = 0;
                for (uint32_t i = 0; i < n_in; i++) {
                    if (is_splice[i]) {
                        Form *lst = temp[i];
                        for (uint32_t j = 0; j < lst->as.list.len; j++) {
                            items[out_idx++] = lst->as.list.items[j];
                        }
                    } else {
                        if (keep_splice_wrapper[i]) {
                            items[out_idx++] = form_unquote_splicing(e->arena, f->as.list.items[i]->span, temp[i]);
                        } else {
                            items[out_idx++] = temp[i];
                        }
                    }
                }
                return form_list(e->arena, f->span, items, n_out);
            }
            Form **items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                items[i] = substitute_params(e, f->as.list.items[i], macro, args);
            }
            return form_list(e->arena, f->span, items, f->as.list.len);
        }
        case F_SYM: {
            /* Check if this symbol is a fixed parameter - if so, replace with corresponding arg */
            for (uint32_t i = 0; i < macro->n_params; i++) {
                Form *param = macro->params[i];
                if (param->tag == F_SYM && param->as.sym == f->as.sym) {
                    return args[i];
                }
            }
            /* Check if this symbol is the rest parameter */
            if (macro->rest_param && macro->rest_param == f->as.sym) {
                return args[macro->n_params]; /* rest list packed at n_params index */
            }
            /* Not a parameter - return as-is */
            return f;
        }
        case F_VEC:
        case F_MAP:
        case F_SET: {
            /* Recursively substitute in vector/map/set items */
            Form **items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                items[i] = substitute_params(e, f->as.list.items[i], macro, args);
            }
            if (f->tag == F_MAP) return form_map(e->arena, f->span, items, f->as.list.len);
            if (f->tag == F_SET) return form_set(e->arena, f->span, items, f->as.list.len);
            return form_vec(e->arena, f->span, items, f->as.list.len);
        }
    }
    return f;
}

/* Phase 6: Expand a macro call with arguments */
Form *elab_expand_macro(Elab *e, MacroDef *macro, Form **args, uint32_t n_args) {
    /* Phase M4: set expansion-origin module so private helper macros of the
     * same module are accessible during this expansion. Restore on every exit. */
    const Symbol *saved_expansion_mod = e->macro_expansion_module;
    e->macro_expansion_module = macro->defining_module_name;

#define EXPAND_RESTORE() (e->macro_expansion_module = saved_expansion_mod)

    /* Check arity */
    if (macro->is_variadic) {
        if (n_args < macro->n_params) {
            diag_emit(DIAG_ERROR, macro->span,
                      "macro '%s' expects at least %u arguments, got %u",
                      macro->name->name, macro->n_params, n_args);
            EXPAND_RESTORE(); return NULL;
        }
        /* Build augmented args: fixed args + packed rest list at index n_params */
        Form **aug_args = (Form **)arena_alloc(e->arena, (macro->n_params + 1) * sizeof(Form *));
        for (uint32_t i = 0; i < macro->n_params; i++) {
            aug_args[i] = args[i];
        }
        /* Pack trailing args into an F_LIST for the rest param */
        uint32_t n_rest = n_args - macro->n_params;
        Form **rest_items = (Form **)arena_alloc(e->arena, n_rest * sizeof(Form *));
        for (uint32_t i = 0; i < n_rest; i++) {
            rest_items[i] = args[macro->n_params + i];
        }
        Span rest_span = n_rest > 0 ? args[macro->n_params]->span : macro->span;
        aug_args[macro->n_params] = form_list(e->arena, rest_span, rest_items, n_rest);
        Form *templ = substitute_params(e, macro->body, macro, aug_args);
        if (!templ) { EXPAND_RESTORE(); return NULL; }
        Form *result = form_contains_ct_builtins(templ) ? elab_eval_macro_form(e, templ) : templ;
        EXPAND_RESTORE(); return result;
    }
    if (n_args != macro->n_params) {
        diag_emit(DIAG_ERROR, macro->span,
                  "macro '%s' expects %u arguments, got %u",
                  macro->name->name, macro->n_params, n_args);
        EXPAND_RESTORE(); return NULL;
    }

    /* Substitute parameters in the macro body */
    Form *templ = substitute_params(e, macro->body, macro, args);
    if (!templ) { EXPAND_RESTORE(); return NULL; }
    Form *result = form_contains_ct_builtins(templ) ? elab_eval_macro_form(e, templ) : templ;
    EXPAND_RESTORE(); return result;

#undef EXPAND_RESTORE
}

/* Phase 6: defmacro — (defmacro name [params...] body...)
 * Defines a macro that will be expanded at compile time.
 * Syntax: (defmacro name [param1 param2 ...] body...)
 * The macro body can use quasiquote/unquote to build the output form.
 * The macro is stored and will be used to expand subsequent calls.
 */
Expr *elab_defmacro(Elab *e, const Form *call) {
    /* Minimum: (defmacro name [params...] body...) */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmacro requires (defmacro name [params...] body...)");
        return NULL;
    }

    /* Parse name */
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defmacro name must be a symbol");
        return NULL;
    }

    /* Check if macro already exists */
    if (elab_lookup_macro(e, name_f->as.sym)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "defmacro: '%s' is already defined", name_f->as.sym->name);
        return NULL;
    }

    /* Parse params */
    Form *params_f = call->as.list.items[2];
    if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defmacro: expected parameter list or vector, got %s", 
                  params_f->tag == F_SYM ? "symbol" : "non-list");
        return NULL;
    }

    /* Extract parameter symbols, detecting optional variadic '& rest' tail. */
    uint32_t n_total = params_f->as.list.len;
    /* Allocate enough space for all params (excluding '&' marker) */
    Form **params = (Form **)arena_alloc(e->arena, n_total * sizeof(Form *));
    uint32_t n_fixed = 0;
    bool is_variadic = false;
    const Symbol *rest_param = NULL;

    for (uint32_t i = 0; i < n_total; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "defmacro: parameter must be a symbol, got %s", 
                      p->tag == F_KEYWORD ? "keyword" : "non-symbol");
            return NULL;
        }
        if (p->as.sym == e->sym_borrow) {
            /* '&' rest-arg marker: the next symbol is the rest param */
            if (i + 1 >= n_total) {
                diag_emit(DIAG_ERROR, p->span,
                          "defmacro: '&' must be followed by a rest parameter name");
                return NULL;
            }
            if (i + 2 < n_total) {
                diag_emit(DIAG_ERROR, params_f->as.list.items[i + 2]->span,
                          "defmacro: only one parameter may follow '&'");
                return NULL;
            }
            Form *rest_f = params_f->as.list.items[i + 1];
            if (rest_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, rest_f->span,
                          "defmacro: rest parameter after '&' must be a symbol");
                return NULL;
            }
            is_variadic = true;
            rest_param = rest_f->as.sym;
            break; /* no more params after & rest */
        }
        params[n_fixed++] = p;
    }

    /* The body is everything after the parameter list */
    uint32_t body_count = call->as.list.len - 3;
    if (body_count == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmacro: expected at least one body expression");
        return NULL;
    }

    /* For now, we only support a single body expression
     * (multi-expression macro bodies would need to be wrapped in do) */
    Form *body = call->as.list.items[3];
    if (body_count > 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmacro: multi-expression bodies not yet supported; wrap in do");
        return NULL;
    }

    /* Create the macro definition */
    MacroDef *macro = (MacroDef *)arena_alloc(e->arena, sizeof(MacroDef));
    macro->name = name_f->as.sym;
    macro->params = params;
    macro->n_params = n_fixed;
    macro->is_variadic = is_variadic;
    macro->rest_param = rest_param;
    macro->body = body;
    macro->span = call->span;
    macro->defining_module_name = e->current_module_name; /* Phase M4 */

    /* Register the macro */
    elab_register_macro(e, macro);

    /* Return nil - defmacro doesn't produce a value */
    return e_nil(e, call->span);
}

/* Phase 6: gensym — (gensym) or (gensym prefix-sym) generates a fresh symbol Form */
Expr *elab_gensym(Elab *e, const Form *call) {
    /* Syntax: (gensym) or (gensym prefix) */
    const char *prefix = "g"; /* default prefix */
    
    if (call->as.list.len >= 2) {
        Form *prefix_f = call->as.list.items[1];
        if (prefix_f->tag == F_SYM) {
            prefix = prefix_f->as.sym->name;
        } else if (prefix_f->tag == F_STR) {
            prefix = prefix_f->as.s.p;
        } else if (prefix_f->tag == F_QUOTE && prefix_f->as.list.len == 1) {
            /* (gensym 'prefix) - get the symbol from the quote */
            Form *quoted = prefix_f->as.list.items[0];
            if (quoted->tag == F_SYM) {
                prefix = quoted->as.sym->name;
            } else {
                diag_emit(DIAG_ERROR, prefix_f->span,
                          "gensym: quoted prefix must be a symbol");
                return NULL;
            }
        } else {
            diag_emit(DIAG_ERROR, prefix_f->span,
                      "gensym: expected symbol, string, or quoted symbol prefix, got %s",
                      prefix_f->tag == F_INT ? "integer" : "other");
            return NULL;
        }
    }
    
    /* Generate a fresh symbol name */
    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "%s_%u", prefix, e->next_gensym_id++);
    const Symbol *fresh_sym = symtab_intern(e->st, strslice(name_buf, (uint32_t)strlen(name_buf)));
    
    /* For phase 6: gensym generates a fresh symbol but doesn't register it. */
    /* The symbol can be used in macro output, but the user must bind it. */
    /* Return a symbol expression - in practice, this will error until gensym */
    /* is used in a proper context (like inside quasiquote in a macro body). */
    Binding *b = binding_new(e, fresh_sym, TYPE_INT, false, false, call->span);
    Expr *out = expr_new(e->arena, EX_VAR, TYPE_INT, call->span);
    out->as.var.binding = b;
    
    return out;
}
