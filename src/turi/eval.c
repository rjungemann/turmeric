/* Phase S0: Tree-walk evaluator for the Turmeric eval core (libturi).
 *
 * Design:
 *  - Each turi_eval call re-elaborates ALL accumulated source plus the new
 *    source, then walks only the NEW top-level expressions.
 *  - Per-call arenas are kept alive in TuriEnv so that closures can hold
 *    Expr* pointers into them indefinitely.
 *  - Variable lookup uses name strings (const char*), not Binding* addresses,
 *    so lookup survives re-elaboration across calls.
 *  - A "return signal" is propagated via env->returning + env->return_value.
 */

/* Platform macros before any system headers */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "eval.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull in all the compiler internal headers from the parent src/ directory.
 * CMake adds src/ to the include path so these resolve correctly. */
#include "arena.h"
#include "buf.h"
#include "builtins.h"
#include "diag.h"
#include "elab.h"
#include "expr.h"
#include "forms.h"
#include "reader.h"
#include "symbols.h"
#include "types.h"

/* -------------------------------------------------------------------------
 * Internal closure representation
 * The public value.h declares TuriClosure as an opaque struct; here we
 * define it properly.
 * ---------------------------------------------------------------------- */

/* Forward-declared in value.c as having fn/captured void*; we redefine: */
typedef struct EvalFrame EvalFrame;

struct TuriClosure {
    FnDef      *fn;        /* FnDef* in some per-call arena (kept alive) */
    EvalFrame  *captured;  /* captured lexical frame (NULL for top-level defn) */
};

/* -------------------------------------------------------------------------
 * Local variable frame (stack-allocated linked list)
 * ---------------------------------------------------------------------- */

typedef struct EvalBinding {
    const char       *name;   /* points into sym_arena — never freed */
    TuriValue         value;
    struct EvalBinding *next;
} EvalBinding;

struct EvalFrame {
    EvalBinding  *bindings;
    EvalFrame    *parent;
};

static EvalFrame *eval_frame_new(EvalFrame *parent) {
    EvalFrame *f = (EvalFrame *)malloc(sizeof(EvalFrame));
    f->bindings = NULL;
    f->parent   = parent;
    return f;
}

static void eval_frame_free(EvalFrame *f) {
    EvalBinding *b = f->bindings;
    while (b) {
        EvalBinding *next = b->next;
        free(b);
        b = next;
    }
    free(f);
}

static void frame_bind(EvalFrame *f, const char *name, TuriValue value) {
    EvalBinding *b = (EvalBinding *)malloc(sizeof(EvalBinding));
    b->name  = name;
    b->value = value;
    b->next  = f->bindings;
    f->bindings = b;
}

/* Returns true and updates the value if the name is found in the frame chain. */
static bool eval_frame_update(EvalFrame *f, const char *name, TuriValue value) {
    for (EvalFrame *cur = f; cur; cur = cur->parent) {
        for (EvalBinding *b = cur->bindings; b; b = b->next) {
            if (strcmp(b->name, name) == 0) {
                b->value = value;
                return true;
            }
        }
    }
    return false;
}

static TuriValue eval_lookup(TuriEnv *env, EvalFrame *frame, const char *name) {
    for (EvalFrame *f = frame; f; f = f->parent) {
        for (EvalBinding *b = f->bindings; b; b = b->next) {
            if (strcmp(b->name, name) == 0) return b->value;
        }
    }
    return turi_env_get(env, name);
}

/* -------------------------------------------------------------------------
 * Builtin dispatch
 * ---------------------------------------------------------------------- */

/* Returns true if the builtin performs I/O (used for sandboxed check). */
static bool is_io_builtin(BuiltinShape shape) {
    switch (shape) {
    case BS_PRINTLN_INT:
    case BS_PRINTLN_FLOAT:
    case BS_PRINTLN_BOOL:
    case BS_PRINTLN_CSTR:
    case BS_PRINTLN_UINT:
    case BS_PRINTLN_FLOAT32:
    case BS_DLOPEN:
    case BS_DLSYM:
    case BS_DLCLOSE:
        return true;
    default:
        return false;
    }
}

static TuriValue eval_builtin(TuriEnv *env, const BuiltinSpec *spec,
                               TuriValue *args, uint32_t n) {
    BuiltinShape shape = spec->shape;

    if (env->sandboxed && is_io_builtin(shape)) {
        return turi_error("eval: I/O not allowed in sandboxed environment");
    }

    switch (shape) {

    case BS_VARIADIC_FOLD: {
        if (n == 0) return turi_error("eval: variadic builtin requires ≥1 arg");
        const char *op = spec->c_op;
        bool is_float = (args[0].tag == TURI_FLOAT);
        if (strcmp(op, "+") == 0) {
            if (!is_float) {
                int64_t acc = args[0].as_int;
                for (uint32_t i = 1; i < n; i++) acc += args[i].as_int;
                return turi_int(acc);
            } else {
                double acc = args[0].as_float;
                for (uint32_t i = 1; i < n; i++) acc += args[i].as_float;
                return turi_float(acc);
            }
        }
        if (strcmp(op, "-") == 0) {
            if (!is_float) {
                int64_t acc = args[0].as_int;
                for (uint32_t i = 1; i < n; i++) acc -= args[i].as_int;
                return turi_int(acc);
            } else {
                double acc = args[0].as_float;
                for (uint32_t i = 1; i < n; i++) acc -= args[i].as_float;
                return turi_float(acc);
            }
        }
        if (strcmp(op, "*") == 0) {
            if (!is_float) {
                int64_t acc = args[0].as_int;
                for (uint32_t i = 1; i < n; i++) acc *= args[i].as_int;
                return turi_int(acc);
            } else {
                double acc = args[0].as_float;
                for (uint32_t i = 1; i < n; i++) acc *= args[i].as_float;
                return turi_float(acc);
            }
        }
        return turi_errorf("eval: unknown variadic builtin '%s'", op);
    }

    case BS_DIV_CHECK: {
        bool is_float = (args[0].tag == TURI_FLOAT);
        if (!is_float) {
            if (args[1].as_int == 0) return turi_error("eval: division by zero");
            return turi_int(args[0].as_int / args[1].as_int);
        } else {
            return turi_float(args[0].as_float / args[1].as_float);
        }
    }

    case BS_BIN_INFIX: {
        const char *op = spec->c_op;
        if (strcmp(op, "==") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   == args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float == args[1].as_float);
            if (args[0].tag == TURI_BOOL)  return turi_bool(args[0].as_bool  == args[1].as_bool);
        }
        if (strcmp(op, "!=") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   != args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float != args[1].as_float);
            if (args[0].tag == TURI_BOOL)  return turi_bool(args[0].as_bool  != args[1].as_bool);
        }
        if (strcmp(op, "<") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   < args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float < args[1].as_float);
        }
        if (strcmp(op, ">") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   > args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float > args[1].as_float);
        }
        if (strcmp(op, "<=") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   <= args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float <= args[1].as_float);
        }
        if (strcmp(op, ">=") == 0) {
            if (args[0].tag == TURI_INT)   return turi_bool(args[0].as_int   >= args[1].as_int);
            if (args[0].tag == TURI_FLOAT) return turi_bool(args[0].as_float >= args[1].as_float);
        }
        if (strcmp(op, "%") == 0) {
            if (args[1].as_int == 0) return turi_error("eval: modulo by zero");
            return turi_int(args[0].as_int % args[1].as_int);
        }
        return turi_errorf("eval: unknown infix builtin '%s'", op);
    }

    case BS_PREFIX_UNARY: {
        const char *op = spec->c_op;
        if (op && strcmp(op, "!") == 0) return turi_bool(!args[0].as_bool);
        return turi_nil();
    }

    case BS_PREFIX_UNARY_FREE:
        /* (drop! x) — no-op in the evaluator */
        return turi_nil();

    case BS_AND_SC: {
        /* Note: args are already evaluated (not truly short-circuit here),
         * but the semantic result is correct for pure boolean expressions. */
        for (uint32_t i = 0; i < n; i++) {
            if (!turi_is_truthy(args[i])) return turi_bool(false);
        }
        return turi_bool(true);
    }

    case BS_OR_SC: {
        for (uint32_t i = 0; i < n; i++) {
            if (turi_is_truthy(args[i])) return turi_bool(true);
        }
        return turi_bool(false);
    }

    case BS_PRINTLN_INT:
        printf("%lld\n", (long long)args[0].as_int);
        return turi_nil();

    case BS_PRINTLN_FLOAT:
        printf("%g\n", args[0].as_float);
        return turi_nil();

    case BS_PRINTLN_BOOL:
        puts(args[0].as_bool ? "true" : "false");
        return turi_nil();

    case BS_PRINTLN_CSTR:
        puts(args[0].as_cstr ? args[0].as_cstr : "");
        return turi_nil();

    case BS_PRINTLN_UINT:
        printf("%llu\n", (unsigned long long)(uint64_t)args[0].as_int);
        return turi_nil();

    case BS_PRINTLN_FLOAT32:
        printf("%.7g\n", args[0].as_float);
        return turi_nil();

    default:
        /* Silently return nil for unsupported builtins (unsafe ops, STM, etc.) */
        return turi_nil();
    }
}

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */

static TuriValue eval_expr(TuriEnv *env, EvalFrame *frame, const Expr *e);

/* -------------------------------------------------------------------------
 * Function application
 * ---------------------------------------------------------------------- */

static TuriValue eval_apply(TuriEnv *env, TuriClosure *cl,
                             TuriValue *args, uint32_t n_args) {
    FnDef *fn = (FnDef *)cl->fn;
    if ((uint32_t)fn->n_params != n_args) {
        return turi_errorf("eval: arity mismatch: %s expects %u args, got %u",
                           fn->binding ? fn->binding->name->name : "<fn>",
                           (unsigned)fn->n_params, (unsigned)n_args);
    }

    /* Build call frame on top of the captured environment */
    EvalFrame *call_frame = eval_frame_new((EvalFrame *)cl->captured);
    for (uint32_t i = 0; i < n_args; i++) {
        frame_bind(call_frame, fn->params[i]->name->name, args[i]);
    }

    /* Evaluate the body; handle early-return signal */
    bool was_returning   = env->returning;
    env->returning       = false;

    TuriValue result = eval_expr(env, call_frame, fn->body);

    TuriValue ret;
    if (env->returning) {
        ret = env->return_value;
        env->returning = was_returning; /* restore caller's return state */
    } else {
        ret = result;
    }

    eval_frame_free(call_frame);
    return ret;
}

/* -------------------------------------------------------------------------
 * Expression evaluator
 * ---------------------------------------------------------------------- */

#define MAX_EVAL_ARGS 64

static TuriValue eval_expr(TuriEnv *env, EvalFrame *frame, const Expr *e) {
    if (!e) return turi_nil();

    /* Propagate signals without evaluating further */
    if (env->returning) return env->return_value;

    switch (e->kind) {

    /* --- Literals -------------------------------------------------------- */
    case EX_NIL_LIT:
        return turi_nil();

    case EX_BOOL_LIT:
        return turi_bool(e->as.b);

    case EX_INT_LIT:
        return turi_int(e->as.i);

    case EX_FLOAT_LIT:
        return turi_float(e->as.f);

    case EX_CSTR_LIT: {
        /* StrSlice — copy to a malloc'd NUL-terminated string */
        char *s = (char *)malloc(e->as.s.len + 1);
        memcpy(s, e->as.s.p, e->as.s.len);
        s[e->as.s.len] = '\0';
        return turi_cstr(s);
    }

    /* --- Variable -------------------------------------------------------- */
    case EX_VAR:
        return eval_lookup(env, frame, e->as.var.binding->name->name);

    /* --- Let ------------------------------------------------------------- */
    case EX_LET: {
        EvalFrame *new_frame = eval_frame_new(frame);
        for (uint32_t i = 0; i < e->as.let_.n; i++) {
            TuriValue v = eval_expr(env, new_frame, e->as.let_.bindings[i].init);
            if (turi_is_error(v)) { eval_frame_free(new_frame); return v; }
            if (env->returning)  { eval_frame_free(new_frame); return env->return_value; }
            frame_bind(new_frame, e->as.let_.bindings[i].binding->name->name, v);
        }
        TuriValue result = eval_expr(env, new_frame, e->as.let_.body);
        eval_frame_free(new_frame);
        return result;
    }

    /* --- If -------------------------------------------------------------- */
    case EX_IF: {
        TuriValue cond = eval_expr(env, frame, e->as.if_.cond);
        if (turi_is_error(cond) || env->returning) return cond;
        if (turi_is_truthy(cond)) {
            return eval_expr(env, frame, e->as.if_.then_);
        } else if (e->as.if_.else_or_null) {
            return eval_expr(env, frame, e->as.if_.else_or_null);
        }
        return turi_nil();
    }

    /* --- Do / Program ---------------------------------------------------- */
    case EX_DO:
    case EX_PROGRAM: {
        Expr   **items = (e->kind == EX_PROGRAM) ? e->as.program.items : e->as.do_.items;
        uint32_t n     = (e->kind == EX_PROGRAM) ? e->as.program.n     : e->as.do_.n;
        TuriValue last = turi_nil();
        for (uint32_t i = 0; i < n; i++) {
            last = eval_expr(env, frame, items[i]);
            if (turi_is_error(last) || env->returning) return last;
        }
        return last;
    }

    /* --- While ----------------------------------------------------------- */
    case EX_WHILE: {
        while (1) {
            TuriValue cond = eval_expr(env, frame, e->as.while_.cond);
            if (turi_is_error(cond) || env->returning) return cond;
            if (!turi_is_truthy(cond)) break;
            TuriValue body = eval_expr(env, frame, e->as.while_.body);
            if (turi_is_error(body) || env->returning) return body;
        }
        return turi_nil();
    }

    /* --- Set ------------------------------------------------------------- */
    case EX_SET: {
        TuriValue v = eval_expr(env, frame, e->as.set_.value);
        if (turi_is_error(v) || env->returning) return v;
        const char *name = e->as.set_.target->name->name;
        if (!eval_frame_update(frame, name, v)) {
            turi_env_set(env, name, v);
        }
        return turi_nil();
    }

    /* --- Def (top-level binding) ---------------------------------------- */
    case EX_DEF: {
        TuriValue v = eval_expr(env, frame, e->as.def_.init);
        if (turi_is_error(v) || env->returning) return v;
        turi_env_set(env, e->as.def_.binding->name->name, v);
        return v;
    }

    /* --- Builtin --------------------------------------------------------- */
    case EX_BUILTIN: {
        TuriValue args[MAX_EVAL_ARGS];
        uint32_t  n = e->as.builtin.n;
        if (n > MAX_EVAL_ARGS)
            return turi_errorf("eval: too many builtin arguments (%u)", n);

        for (uint32_t i = 0; i < n; i++) {
            args[i] = eval_expr(env, frame, e->as.builtin.args[i]);
            if (turi_is_error(args[i]) || env->returning) return args[i];
        }
        return eval_builtin(env, e->as.builtin.spec, args, n);
    }

    /* --- Named function definition (defn) -------------------------------- */
    case EX_FN_DEF: {
        TuriClosure *cl = (TuriClosure *)malloc(sizeof(TuriClosure));
        cl->fn       = e->as.fn_def_.fn;
        cl->captured = NULL; /* top-level defn has no captured environment */
        TuriValue v  = turi_closure(cl);
        turi_env_set(env, e->as.fn_def_.fn->binding->name->name, v);
        return v;
    }

    /* --- Anonymous function (fn) ---------------------------------------- */
    case EX_FN: {
        TuriClosure *cl = (TuriClosure *)malloc(sizeof(TuriClosure));
        cl->fn       = e->as.fn_.fn;
        cl->captured = frame; /* capture lexical scope */
        return turi_closure(cl);
    }

    /* --- Function call --------------------------------------------------- */
    case EX_CALL: {
        TuriValue fn_val;
        if (e->as.call_.fn_binding) {
            fn_val = eval_lookup(env, frame,
                                 e->as.call_.fn_binding->name->name);
        } else if (e->as.call_.fn_expr) {
            fn_val = eval_expr(env, frame, e->as.call_.fn_expr);
        } else {
            return turi_error("eval: call with no function");
        }
        if (turi_is_error(fn_val) || env->returning) return fn_val;
        if (fn_val.tag != TURI_CLOSURE)
            return turi_errorf("eval: expected function, got tag %d", fn_val.tag);

        TuriValue args[MAX_EVAL_ARGS];
        uint32_t  n_args = e->as.call_.n_args;
        if (n_args > MAX_EVAL_ARGS)
            return turi_errorf("eval: too many call arguments (%u)", n_args);

        for (uint32_t i = 0; i < n_args; i++) {
            args[i] = eval_expr(env, frame, e->as.call_.args[i]);
            if (turi_is_error(args[i]) || env->returning) return args[i];
        }
        return eval_apply(env, fn_val.as_closure, args, n_args);
    }

    /* --- Early return ---------------------------------------------------- */
    case EX_RETURN: {
        TuriValue v = turi_nil();
        if (e->as.return_.value) {
            v = eval_expr(env, frame, e->as.return_.value);
            if (turi_is_error(v)) return v;
        }
        env->returning    = true;
        env->return_value = v;
        return v;
    }

    /* --- Typeclass/instance definitions — no runtime action -------------- */
    case EX_TYPECLASS_DEF:
    case EX_INSTANCE_DEF:
        return turi_nil();

    /* --- Module — evaluate body ----------------------------------------- */
    case EX_DEFMODULE: {
        DefModule *mod = e->as.defmodule_.mod;
        TuriValue  last = turi_nil();
        for (uint32_t i = 0; i < mod->n_body; i++) {
            last = eval_expr(env, frame, mod->body[i]);
            if (turi_is_error(last) || env->returning) return last;
        }
        return last;
    }

    /* --- Everything else — silently return nil for Phase S0 -------------- */
    default:
        return turi_nil();
    }
}

/* -------------------------------------------------------------------------
 * turi_eval: public entry point
 * ---------------------------------------------------------------------- */

TuriValue turi_eval(TuriEnv *env, const char *src) {
    if (!env || !src) return turi_error("turi_eval: null argument");

    /* 1. Build combined source: all prior definitions + new source. */
    Buf combined;
    buf_init(&combined);
    if (env->src_acc.len > 0) {
        buf_write(&combined, env->src_acc.data, env->src_acc.len);
        buf_putc(&combined, '\n');
    }
    buf_puts(&combined, src);

    /* 2. Create a new per-call arena and link it into env. */
    ArenaNode *node = (ArenaNode *)malloc(sizeof(ArenaNode));
    arena_init(&node->arena, 0);
    node->next      = env->eval_arenas;
    env->eval_arenas = node;
    Arena *eval_arena = &node->arena;

    /* 3. Copy the combined source into the arena so it survives this call. */
    size_t src_len  = combined.len;
    char  *src_copy = arena_strdup(eval_arena, combined.data, src_len);
    buf_free(&combined);

    /* 4. Reset diagnostics; register the eval source file. */
    diag_reset();

    SourceFile *sfile = (SourceFile *)arena_alloc(eval_arena, sizeof(SourceFile));
    sfile->path        = "<eval>";
    sfile->src         = src_copy;
    sfile->len         = src_len;
    sfile->file_id     = 0;
    sfile->reader_type = READER_TURMERIC;
    diag_register_file(sfile);

    /* 5. Parse. */
    uint32_t  nforms = 0;
    Form    **forms  = read_all(eval_arena, &env->st, sfile, &nforms);
    if (!forms || diag_had_error()) {
        return turi_error("parse error");
    }

    /* 6. Elaborate (read-only path: no borrow-check, no CPS, no emit). */
    Expr *prog = elaborate_program(eval_arena, &env->st,
                                   forms, nforms,
                                   /*stdlib_prefix=*/0,
                                   /*module_base_dir=*/".",
                                   /*separate_compilation=*/false,
                                   /*out_tc_env=*/NULL);
    if (!prog || diag_had_error()) {
        return turi_error("elaboration error");
    }

    /* 7. Evaluate only the NEW top-level expressions. */
    uint32_t prior = env->prior_toplevel;
    uint32_t total = prog->as.program.n;

    TuriValue last = turi_nil();
    for (uint32_t i = prior; i < total; i++) {
        last = eval_expr(env, NULL, prog->as.program.items[i]);
        /* Clear any dangling return signal at the top level */
        if (env->returning) {
            last = env->return_value;
            env->returning = false;
        }
        if (turi_is_error(last)) break;
    }

    /* 8. Update accumulated state only on success. */
    if (!turi_is_error(last)) {
        /* Append new source to accumulator */
        if (env->src_acc.len > 0) buf_putc(&env->src_acc, '\n');
        buf_puts(&env->src_acc, src);
        env->prior_toplevel = total;
    }

    return last;
}

/* -------------------------------------------------------------------------
 * turi_eval_file: read a file and evaluate it
 * ---------------------------------------------------------------------- */

TuriValue turi_eval_file(TuriEnv *env, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return turi_errorf("cannot open '%s': %s", path, strerror(errno));
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return turi_error("fseek failed"); }
    long size = ftell(f);
    if (size < 0) { fclose(f); return turi_error("ftell failed"); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return turi_error("fseek failed"); }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return turi_error("out of memory"); }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return turi_error("read error"); }
    buf[size] = '\0';

    TuriValue v = turi_eval(env, buf);
    free(buf);
    return v;
}

/* -------------------------------------------------------------------------
 * turi_init / turi_value_repr
 * ---------------------------------------------------------------------- */

void turi_init(bool use_color) {
    diag_init(use_color);
}

void turi_value_repr(char *buf, size_t cap, TuriValue v) {
    if (!buf || cap == 0) return;
    switch (v.tag) {
    case TURI_NIL:
        snprintf(buf, cap, "nil");
        break;
    case TURI_BOOL:
        snprintf(buf, cap, "%s", v.as_bool ? "true" : "false");
        break;
    case TURI_INT:
        snprintf(buf, cap, "%lld", (long long)v.as_int);
        break;
    case TURI_FLOAT:
        snprintf(buf, cap, "%g", v.as_float);
        break;
    case TURI_CSTR:
        snprintf(buf, cap, "\"%s\"", v.as_cstr ? v.as_cstr : "");
        break;
    case TURI_CLOSURE: {
        FnDef *fn = v.as_closure ? (FnDef *)v.as_closure->fn : NULL;
        if (fn && fn->binding) {
            snprintf(buf, cap, "#<fn %s>", fn->binding->name->name);
        } else {
            snprintf(buf, cap, "#<fn>");
        }
        break;
    }
    case TURI_ERROR:
        snprintf(buf, cap, "#<error: %s>", v.as_error ? v.as_error : "");
        break;
    }
}
