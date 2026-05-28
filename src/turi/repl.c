/* Phase S1: Interactive REPL for `tur repl`.
 *
 * Features:
 *  - libedit/readline line editing + persistent history (~/.tur_history)
 *  - Multi-line continuation prompt (`..`) while parentheses are unbalanced
 *  - Meta-commands: :help  :quit/:q  :type <expr>  :doc <sym>  :reload <file>
 *  - Colour diagnostics when stderr is a terminal (reuses src/diag.c)
 *  - Pretty-printer for TuriValue (extends turi_value_repr)
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

#include "repl.h"
#include "eval.h"
#include "spice_loader.h"  /* RP3: auto-discover + load the enclosing spice */
#include "ffi_thunk.h"     /* RP4: install per-export TuriNativeFn bindings */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* libedit readline-compatible API (ships with macOS and most Linux distros).
 * Gracefully degrade to fgets when not available at compile time. */
#ifdef TURI_HAVE_EDITLINE
#  include <editline/readline.h>
#endif

#ifdef TURI_HAVE_EDITLINE
/* E11: tab-completion — current REPL environment for the completion generator.
 * Updated whenever the env is recreated (e.g. after :reset). */
static TuriEnv *g_completion_env = NULL;
#endif

/* Compiler internals (CMake adds src/ to the include path) */
#include "arena.h"
#include "buf.h"
#include "diag.h"
#include "elab.h"
#include "expr.h"
#include "forms.h"
#include "reader.h"
#include "symbols.h"
#include "types.h"
/* Tutorial system */
#include "tutorial.h"

/* -------------------------------------------------------------------------
 * Paren-balance counter — drives multi-line continuation
 * ---------------------------------------------------------------------- */

/* Count the paren imbalance in a string (positive = more open than close).
 * Ignores parens inside string literals and line comments. */
static int paren_balance(const char *s) {
    int depth = 0;
    bool in_str = false;
    for (; *s; s++) {
        if (in_str) {
            if (*s == '\\' && s[1]) { s++; continue; }
            if (*s == '"') in_str = false;
            continue;
        }
        if (*s == '"') { in_str = true; continue; }
        if (*s == ';') break; /* line comment */
        if (*s == '(' || *s == '[' || *s == '{') depth++;
        if (*s == ')' || *s == ']' || *s == '}') depth--;
    }
    return depth;
}

/* -------------------------------------------------------------------------
 * E11: Tab-completion generator (editline only)
 * ---------------------------------------------------------------------- */

#ifdef TURI_HAVE_EDITLINE
/* Known REPL meta-commands for colon-prefix completion. */
static const char *const k_meta_cmds[] = {
    ":help", ":quit", ":q",
    ":type", ":doc", ":reload", ":reset",
    ":tutorial", ":next", ":prev", ":hint", ":skip",
    ":quit-tutorial", ":tutorial-progress",
    NULL
};

/* readline/editline generator: called with state=0 on first call for a
 * given prefix, then state>0 for subsequent calls.  Returns a malloc'd
 * match string or NULL when the list is exhausted. */
static char *tur_completion_generator(const char *text, int state) {
    static int meta_idx;
    static EnvBinding *cur_binding;
    size_t tlen = strlen(text);

    if (state == 0) {
        meta_idx   = 0;
        cur_binding = g_completion_env ? g_completion_env->globals : NULL;
    }

    if (text[0] == ':') {
        /* Complete meta-commands */
        while (k_meta_cmds[meta_idx]) {
            const char *m = k_meta_cmds[meta_idx++];
            if (strncmp(m, text, tlen) == 0) return strdup(m);
        }
        return NULL;
    }

    /* Complete environment bindings */
    while (cur_binding) {
        const char *name = cur_binding->name;
        cur_binding = cur_binding->next;
        if (strncmp(name, text, tlen) == 0) return strdup(name);
    }
    return NULL;
}
#endif /* TURI_HAVE_EDITLINE */

/* -------------------------------------------------------------------------
 * Line input abstraction (editline when available, fgets fallback)
 * ---------------------------------------------------------------------- */

/* Read one line with the given prompt.  Returns a malloc'd string (caller
 * must free) or NULL on EOF/error.  Uses add_history when editline is
 * available and the line is non-empty. */
static char *repl_readline(const char *prompt) {
#ifdef TURI_HAVE_EDITLINE
    char *line = readline(prompt);
    if (line && line[0] != '\0') add_history(line);
    return line; /* readline returns malloc'd memory */
#else
    if (isatty(STDIN_FILENO)) { printf("%s", prompt); fflush(stdout); }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
    return strdup(buf);
#endif
}

/* -------------------------------------------------------------------------
 * History file persistence
 * ---------------------------------------------------------------------- */

static char *history_path(void) {
#ifdef TURI_HAVE_EDITLINE
    const char *home = getenv("HOME");
    if (!home) return NULL;
    size_t len = strlen(home) + sizeof("/.tur_history");
    char *p = (char *)malloc(len);
    if (!p) return NULL;
    snprintf(p, len, "%s/.tur_history", home);
    return p;
#else
    return NULL;
#endif
}

/* -------------------------------------------------------------------------
 * Pretty-printer
 * Extends turi_value_repr with coloured output to a FILE* when appropriate.
 * ---------------------------------------------------------------------- */

/* ANSI colours — used only when output is a terminal. */
#define COL_RESET  "\033[0m"
#define COL_INT    "\033[36m"   /* cyan    */
#define COL_FLOAT  "\033[36m"
#define COL_BOOL   "\033[33m"   /* yellow  */
#define COL_CSTR   "\033[32m"   /* green   */
#define COL_FN     "\033[35m"   /* magenta */
#define COL_NIL    "\033[90m"   /* grey    */
#define COL_ERR    "\033[31m"   /* red     */

static void repl_print_value(TuriValue v, bool use_color) {
    char repr[512];
    turi_value_repr(repr, sizeof(repr), v);

    if (!use_color) {
        printf("=> %s\n", repr);
        return;
    }

    const char *col = COL_RESET;
    switch (v.tag) {
        case TURI_NIL:     col = COL_NIL;   break;
        case TURI_BOOL:    col = COL_BOOL;  break;
        case TURI_INT:     col = COL_INT;   break;
        case TURI_FLOAT:   col = COL_FLOAT; break;
        case TURI_CSTR:    col = COL_CSTR;  break;
        case TURI_CLOSURE:     col = COL_FN;    break;
        case TURI_ERROR:       col = COL_ERR;   break;
        case TURI_EFFECT_CONT: col = COL_FN;    break;
        case TURI_STRUCT:      col = COL_RESET; break;
        case TURI_THROW:       col = COL_ERR;   break;
        case TURI_FUTURE:      col = COL_RESET; break;
        case TURI_REF:         col = COL_RESET; break;
        case TURI_STRUCT_TYPE: col = COL_NIL;   break;
    }
    printf("=> %s%s%s\n", col, repr, COL_RESET);
}

/* -------------------------------------------------------------------------
 * :type <expr>  — elaborate and print the inferred type without evaluating
 * ---------------------------------------------------------------------- */

static void cmd_type(TuriEnv *env, const char *expr_src) {
    /* Build combined source so previous definitions are visible */
    Buf combined;
    buf_init(&combined);
    if (env->src_acc.len > 0) {
        buf_write(&combined, env->src_acc.data, env->src_acc.len);
    }
    buf_puts(&combined, expr_src);
    buf_putc(&combined, '\0');

    Arena    arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    diag_reset();
    SourceFile sfile;
    sfile.path        = "<type>";
    sfile.src         = combined.data;
    sfile.len         = combined.len - 1;
    sfile.file_id     = 0;
    sfile.reader_type = env->reader_type;
    diag_register_file(&sfile);

    uint32_t nforms = 0;
    /* RM Q#5: share the session registry with `:type` so it sees macros
     * defined earlier in the REPL. */
    Form **forms = read_all_with_registry(&arena, &st, &sfile,
                                          env->reader_macros, &nforms);
    if (!forms || diag_had_error()) goto cleanup;

    {
        Expr *prog = elaborate_program(&arena, &st, forms, nforms,
                                       /*stdlib_prefix=*/0, ".",
                                       /*separate_compilation=*/false,
                                       /*sandboxed=*/false,
                                       /*tc_env=*/NULL,
                                       /*include_dirs=*/NULL,
                                       /*n_include_dirs=*/0,
                                       /*out_n_fsd=*/NULL,
                                       /* RM transitive: `:type` shares
                                        * the session registry. */
                                       env->reader_macros);
        if (!prog || diag_had_error()) goto cleanup;

        /* The last top-level item is the expression we care about */
        uint32_t n = prog->as.program.n;
        if (n == 0) { printf(":type — empty expression\n"); goto cleanup; }

        Expr *last = prog->as.program.items[n - 1];
        printf(": %s\n", type_name(last->type));
    }

cleanup:
    arena_free(&arena);
    buf_free(&combined);
}

/* -------------------------------------------------------------------------
 * :doc <sym>  — print a brief description for known builtins / env bindings
 * ---------------------------------------------------------------------- */

const char *turi_doc_lookup_builtin(const char *sym) {
    static const struct { const char *name; const char *doc; } docs[] = {
        /* Arithmetic */
        {"+",        "(+ a b ...) -- add numbers"},
        {"-",        "(- a b ...) -- subtract numbers"},
        {"*",        "(* a b ...) -- multiply numbers"},
        {"/",        "(/ a b) -- divide numbers"},
        {"mod",      "(mod a b) -- integer remainder"},
        /* Comparison */
        {"=",        "(= a b) -- equality"},
        {"!=",       "(!= a b) -- inequality"},
        {"<",        "(< a b) -- less-than"},
        {">",        "(> a b) -- greater-than"},
        {"<=",       "(<= a b) -- less-than-or-equal"},
        {">=",       "(>= a b) -- greater-than-or-equal"},
        /* Logic */
        {"not",      "(not b) -- boolean negation"},
        {"and",      "(and a b ...) -- short-circuit logical and"},
        {"or",       "(or a b ...) -- short-circuit logical or"},
        /* I/O */
        {"println",  "(println x) -- print value with trailing newline"},
        {"print",    "(print x) -- print value without trailing newline"},
        /* Core special forms */
        {"let",      "(let [x v ...] body) -- bind local variables in scope of body"},
        {"if",       "(if cond then else) -- conditional: evaluates then or else branch"},
        {"do",       "(do e1 e2 ...) -- evaluate expressions in sequence, return last"},
        {"defn",     "(defn name [p1 :T1 ...] :Ret body) -- define a named function"},
        {"fn",       "(fn [p1 :T1 ...] :Ret body) -- anonymous function (lambda)"},
        {"def",      "(def name value) -- bind name to value at top level"},
        {"while",    "(while cond body) -- loop while cond is true"},
        {"set!",     "(set! var value) -- mutate an existing variable binding"},
        {"quote",    "(quote x) -- return x unevaluated; shorthand: 'x"},
        {"return",   "(return value) -- early return from a function"},
        {"defer",    "(defer body) -- run body when current scope exits"},
        /* Pattern matching and data */
        {"match",    "(match val (Pattern body) ...) -- destructure and branch on value"},
        {"defstruct","(defstruct Name [field :Type ...]) -- define a named product type"},
        {"defdata",  "(defdata Name (Ctor) (Ctor :T) ...) -- define an algebraic data type"},
        {"defgadt",  "(defgadt Name [a] (Ctor :T) ...) -- define a generalized ADT"},
        {"deftype",  "(deftype Alias ActualType) -- define a type alias"},
        /* Macros */
        {"defmacro", "(defmacro name [args] body) -- define a syntax macro"},
        {"when",     "(when cond body ...) -- execute body if cond is true, else nil"},
        {"unless",   "(unless cond body ...) -- execute body if cond is false, else nil"},
        {"cond",     "(cond (test expr) ... (else expr)) -- multi-branch conditional"},
        {"for",      "(for [x seq] body) -- iterate over a sequence"},
        /* Modules */
        {"defmodule","(defmodule Name (export ...) body ...) -- define a module"},
        {"import",   "(import module/name :as alias) -- import a module (inside defmodule)"},
        /* Typeclasses */
        {"defclass", "(defclass Name [param] (method :Type) ...) -- define a typeclass"},
        {"definstance","(definstance ClassName TypeName method-impls ...) -- implement a typeclass"},
        /* Effects */
        {"defeffect","(defeffect Name (op :Type) ...) -- define an algebraic effect"},
        {"perform",  "(perform effect/op args ...) -- perform an effect operation"},
        {"handle",   "(handle expr (effect/op args k) body ...) -- handle effects"},
        {"resume",   "(resume k value) -- resume a delimited continuation"},
        /* Async */
        {"async",    "(async body) -- create an async computation"},
        {"await",    "(await future) -- wait for an async computation to complete"},
        /* Error handling */
        {"try",      "(try body (catch err handler)) -- catch runtime errors"},
        {"catch",    "catch -- error handler clause in (try ...)"},
        {"throw",    "(throw msg) -- raise a runtime error"},
        /* Dynamic vars */
        {"defdynamic","(defdynamic *name* :Type init) -- define a dynamic (thread-local) variable"},
        {"let-dyn",  "(let-dyn [*name* val] body) -- bind dynamic variable within scope"},
        {NULL, NULL}
    };
    for (int i = 0; docs[i].name; i++) {
        if (strcmp(sym, docs[i].name) == 0)
            return docs[i].doc;
    }
    return NULL;
}

static void cmd_doc(TuriEnv *env, const char *sym) {
    /* Check if the symbol is bound in the env */
    TuriValue v = turi_env_get(env, sym);
    if (!turi_is_error(v)) {
        char repr[256];
        turi_value_repr(repr, sizeof(repr), v);
        printf("%s = %s\n", sym, repr);
        return;
    }

    const char *d = turi_doc_lookup_builtin(sym);
    if (d) {
        printf("%s\n", d);
        return;
    }

    printf("no documentation for '%s'\n", sym);
}

/* -------------------------------------------------------------------------
 * :reload <file>  — evaluate a source file into the current environment
 * ---------------------------------------------------------------------- */

static void cmd_reload(TuriEnv *env, const char *path) {
    TuriValue result = turi_eval_file(env, path);
    if (turi_is_error(result)) {
        const char *msg = turi_error_message(result);
        if (msg &&
            strcmp(msg, "parse error") != 0 &&
            strcmp(msg, "elaboration error") != 0) {
            fprintf(stderr, "reload error: %s\n", msg);
        }
    } else {
        printf("reloaded %s\n", path);
    }
}

/* -------------------------------------------------------------------------
 * Help text
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * 2h: detect type-level definition forms to show a sentinel instead of nil
 * ---------------------------------------------------------------------- */

/* Scan src for a type-level definition like (defstruct Foo ...).
 * Returns true and writes kind + name if found, false otherwise. */
static bool detect_type_form(const char *src,
                              char *kind, size_t ksz,
                              char *name, size_t nsz) {
    /* Skip leading whitespace and optional opening paren */
    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r') src++;
    if (*src == '(') src++;
    while (*src == ' ' || *src == '\t') src++;

    static const struct { const char *kw; const char *kind; } forms[] = {
        {"defstruct ",    "struct-type"},
        {"defdata ",      "adt-type"},
        {"defgadt ",      "adt-type"},
        {"deftype ",      "type-alias"},
        {"defmacro ",     "macro"},
        {"defeffect ",    "effect"},
        {"defclass ",     "typeclass"},
        {"definstance ",  "instance"},
        {NULL, NULL}
    };

    for (int i = 0; forms[i].kw; i++) {
        size_t klen = strlen(forms[i].kw);
        if (strncmp(src, forms[i].kw, klen) != 0) continue;

        snprintf(kind, ksz, "%s", forms[i].kind);
        const char *p = src + klen;
        while (*p == ' ' || *p == '\t') p++;

        if (strcmp(forms[i].kind, "instance") == 0) {
            /* (definstance Typeclass Type ...) — include both names */
            char tc[32] = {0}, ty[32] = {0};
            int ti = 0;
            while (*p && *p != ' ' && *p != ')' && ti < 31) tc[ti++] = *p++;
            while (*p == ' ') p++;
            int yi = 0;
            while (*p && *p != ' ' && *p != ')' && yi < 31) ty[yi++] = *p++;
            snprintf(name, nsz, "%s/%s", tc, ty);
        } else {
            size_t ni = 0;
            while (*p && *p != ' ' && *p != ')' && *p != ']' && *p != '[' && ni < nsz - 1)
                name[ni++] = *p++;
            name[ni] = '\0';
        }
        return name[0] != '\0';
    }
    return false;
}

static void print_help(void) {
    printf(
        "Meta-commands:\n"
        "  :help               show this help\n"
        "  :quit  :q           exit the REPL\n"
        "  :type <expr>        print inferred type without evaluating\n"
        "  :doc  <sym>         print documentation for a symbol or builtin\n"
        "  :reload <file>      evaluate a .tur file into the current session\n"
        "  :reset              clear session and start fresh\n"
        "\n"
        "Tutorial commands:\n"
        "  :tutorial              list available tutorials\n"
        "  :tutorial <name>       start a tutorial\n"
        "  :tutorial <name> <n>   start a tutorial at step n\n"
        "  :next                  go to next step\n"
        "  :prev                  go to previous step\n"
        "  :hint                  show hint for current step\n"
        "  :skip                  skip current step\n"
        "  :quit-tutorial         exit tutorial mode\n"
        "  :tutorial-progress     show progress in current tutorial\n"
        "\n"
        "Expressions are evaluated and the result printed.\n"
        "Definitions (defn, def) persist across expressions.\n"
        "Multi-line input: keep typing when parentheses are open; use\n"
        "  an empty line to cancel an incomplete expression.\n"
    );
}

/* -------------------------------------------------------------------------
 * Main REPL entry point
 * ---------------------------------------------------------------------- */

/* Tutorial state for the REPL session */
static TutorialState *g_tutorial_state = NULL;

/* -------------------------------------------------------------------------
 * Tutorial meta-commands
 * ---------------------------------------------------------------------- */

static void print_tutorial_help(void) {
    printf(
        "Tutorial meta-commands:\n"
        "  :tutorial              list available tutorials\n"
        "  :tutorial <name>       start a tutorial\n"
        "  :tutorial <name> <n>   start a tutorial at step n\n"
        "  :next                  go to next step\n"
        "  :prev                  go to previous step\n"
        "  :hint                  show hint for current step\n"
        "  :skip                  skip current step\n"
        "  :quit-tutorial         exit tutorial mode\n"
        "  :tutorial-progress     show progress in current tutorial\n"
    );
}

static void cmd_tutorial_list(void) {
    int count = 0;
    Tutorial **tutorials = tutorial_load_all(&count);
    
    if (count == 0) {
        printf("No tutorials available.\n");
        return;
    }
    
    printf("Available tutorials:\n");
    for (int i = 0; i < count; i++) {
        Tutorial *t = tutorials[i];
        char completed_marker = tutorial_is_completed(g_tutorial_state, t->id) ? 'v' : ' ';
        printf("  %c %s - %s (%d steps, difficulty %d/5)\n",
               completed_marker, t->id, t->description, t->step_count, t->difficulty);
    }
}

static void cmd_tutorial_start(const char *name, int start_step) {
    if (!name || name[0] == '\0') {
        printf(":tutorial requires a tutorial name\n");
        return;
    }
    
    if (!tutorial_start(g_tutorial_state, name)) {
        printf("unknown tutorial '%s' — use :tutorial to list available tutorials\n", name);
        return;
    }
    
    if (start_step > 0) {
        tutorial_jump(g_tutorial_state, start_step - 1);
    }
    
    Tutorial *t = tutorial_get_current_tutorial(g_tutorial_state);
    TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
    
    printf("Starting tutorial: %s\n", t->title);
    printf("Step %d/%d: %s\n", tutorial_get_current_step_index(g_tutorial_state) + 1,
           tutorial_get_step_count(g_tutorial_state), step->title);
    printf("Instruction: %s\n", step->instruction);
}

static void cmd_tutorial_next(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode. Use :tutorial <name> to start a tutorial.\n");
        return;
    }
    
    if (tutorial_next(g_tutorial_state)) {
        TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
        printf("Step %d/%d: %s\n", tutorial_get_current_step_index(g_tutorial_state) + 1,
               tutorial_get_step_count(g_tutorial_state), step->title);
        printf("Instruction: %s\n", step->instruction);
    } else {
        printf("You have completed all steps in this tutorial!\n");
        printf("Use :tutorial-progress to see your progress.\n");
    }
}

static void cmd_tutorial_prev(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode. Use :tutorial <name> to start a tutorial.\n");
        return;
    }
    
    if (tutorial_prev(g_tutorial_state)) {
        TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
        printf("Step %d/%d: %s\n", tutorial_get_current_step_index(g_tutorial_state) + 1,
               tutorial_get_step_count(g_tutorial_state), step->title);
        printf("Instruction: %s\n", step->instruction);
    } else {
        printf("Already at the first step.\n");
    }
}

static void cmd_tutorial_hint(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode. Use :tutorial <name> to start a tutorial.\n");
        return;
    }
    
    TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
    int hint_count = tutorial_get_hint_count(step);
    
    if (hint_count == 0) {
        printf("No hints available for this step.\n");
        return;
    }
    
    /* Show all hints */
    printf("Hints:\n");
    for (int i = 0; i < hint_count; i++) {
        const char *hint = tutorial_get_hint(step, i);
        printf("  %d. %s\n", i + 1, hint);
    }
}

static void cmd_tutorial_skip(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode. Use :tutorial <name> to start a tutorial.\n");
        return;
    }
    
    TutorialStep *step = tutorial_get_current_step(g_tutorial_state);

    printf("Skipping step: %s\n", step->title);
    cmd_tutorial_next();
}

static void cmd_tutorial_quit(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode.\n");
        return;
    }
    
    Tutorial *t = tutorial_get_current_tutorial(g_tutorial_state);
    printf("Exiting tutorial: %s\n", t->title);
    g_tutorial_state->in_tutorial = false;
    g_tutorial_state->tutorial = NULL;
    g_tutorial_state->current_step = 0;
}

static void cmd_tutorial_progress(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode.\n");
        return;
    }
    
    Tutorial *t = tutorial_get_current_tutorial(g_tutorial_state);
    int progress = tutorial_get_progress(g_tutorial_state);
    int current = tutorial_get_current_step_index(g_tutorial_state) + 1;
    int total = tutorial_get_step_count(g_tutorial_state);
    
    printf("Tutorial: %s\n", t->title);
    printf("Progress: %d%% (%d/%d steps)\n", progress, current, total);
    
    TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
    printf("Current step: %s\n", step->title);
    printf("Instruction: %s\n", step->instruction);
}

/* -------------------------------------------------------------------------
 * Tutorial integration with evaluation
 * ---------------------------------------------------------------------- */

/* Check if the input matches the current tutorial step */
static bool check_tutorial_step(TuriEnv *env, const char *input) {
    if (!g_tutorial_state->in_tutorial) return false;
    
    TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
    
    if (tutorial_is_step_complete(g_tutorial_state, input)) {
        printf("✓ %s\n", step->success_message);
        
        /* If there's a verify expression, evaluate it */
        if (step->verify_expr) {
            TuriValue result = turi_eval(env, step->verify_expr);
            if (turi_is_error(result)) {
                printf("Verification failed: %s\n", turi_error_message(result));
            } else {
                char repr[256];
                turi_value_repr(repr, sizeof(repr), result);
                printf("Verification: %s\n", repr);
            }
        }
        
        /* Mark step as completed */
        g_tutorial_state->step_completed = true;
        
        /* Auto-advance to next step */
        if (tutorial_next(g_tutorial_state)) {
            TutorialStep *next_step = tutorial_get_current_step(g_tutorial_state);
            printf("\nStep %d/%d: %s\n", tutorial_get_current_step_index(g_tutorial_state) + 1,
                   tutorial_get_step_count(g_tutorial_state), next_step->title);
            printf("Instruction: %s\n", next_step->instruction);
        } else {
            /* Completed all steps */
            Tutorial *t = tutorial_get_current_tutorial(g_tutorial_state);
            printf("\n✓ Tutorial '%s' completed!\n", t->title);
            tutorial_mark_completed(g_tutorial_state);
            g_tutorial_state->in_tutorial = false;
            g_tutorial_state->tutorial = NULL;
        }
        
        return true;
    }
    
    return false;
}

int turi_repl_run(bool watch_mode) {
    bool use_color = isatty(STDOUT_FILENO) && isatty(STDERR_FILENO);

    turi_init(use_color);
    
    /* Initialize tutorial system */
    tutorial_init();
    g_tutorial_state = tutorial_state_new();
    if (!g_tutorial_state) {
        fprintf(stderr, "tur repl: failed to initialize tutorial state\n");
    }

#ifndef TUR_VERSION
#define TUR_VERSION "unknown"
#endif
    if (use_color) {
        printf(COL_BOOL
               "\xe2\x96\x97            \xe2\x96\x98  \n"
               "\xe2\x96\x9c\xe2\x96\x98\xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x9b\xe2\x96\x9b\xe2\x96\x8c\xe2\x96\x88\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\n"
               "\xe2\x96\x90\xe2\x96\x96\xe2\x96\x99\xe2\x96\x8c\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\n"
               COL_RESET "\n");
    } else {
        printf("\xe2\x96\x97            \xe2\x96\x98  \n"
               "\xe2\x96\x9c\xe2\x96\x98\xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x9b\xe2\x96\x9b\xe2\x96\x8c\xe2\x96\x88\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\n"
               "\xe2\x96\x90\xe2\x96\x96\xe2\x96\x99\xe2\x96\x8c\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\n"
               "\n");
    }
    printf("Turmeric v" TUR_VERSION "  (type :help for help, :quit to exit)\n");
    fflush(stdout);

    TuriEnv *env = turi_env_new();
    if (!env) {
        fprintf(stderr, "tur repl: failed to create eval environment\n");
        return 1;
    }

    /* RP5: register `(reload)` unconditionally so users always have
     * a callable handle, even outside a spice project (the native
     * surfaces a "no spice loaded" message in that case rather than
     * an unbound-variable error). */
    tur_ffi_register_reload_native(env);

    /* RP3: auto-discover an enclosing spice project and load its
     * shared library. Skipped silently when:
     *   - the user sets TUR_NO_AUTO_SPICE=1, or
     *   - no build.tur exists walking up from cwd.
     * On hard error (failed build, dlopen, manifest parse) the loader
     * prints the diagnostic itself; we still continue to a usable
     * pure-Turmeric REPL so the user can debug. The subprocess invokes
     * the `tur` binary on PATH; override via TUR_BIN. */
    const char *no_auto = getenv("TUR_NO_AUTO_SPICE");
    if (!no_auto || strcmp(no_auto, "1") != 0) {
        TurSpiceImage *img = NULL;
        int srv = tur_spice_image_load(".", getenv("TUR_BIN"), &img);
        if (srv == 0 && img) {
            env->spice_image = img;
            /* RP4: register a TuriNativeFn per export so the user can
             * call spice defns directly at the prompt (both bare and
             * `<module>/<defn>` qualified names). The TuriEnv now owns
             * both the image and the binding shims. */
            uint32_t n_exports = tur_spice_image_count(img);
            uint32_t n_bound = tur_ffi_install_spice_bindings(env, img);
            (void)n_bound;
            printf("Loaded spice from %s (%u export%s)\n",
                   tur_spice_image_root(img),
                   n_exports,
                   n_exports == 1 ? "" : "s");
            fflush(stdout);
        }
        /* srv == 1 (no project) and srv == -1 (hard error, already
         * surfaced) both leave env->spice_image NULL: the REPL behaves
         * like a pure-Turmeric session. */
    }

    /* Load history */
    char *hist_path = history_path();
#ifdef TURI_HAVE_EDITLINE
    using_history();
    stifle_history(1000);
    if (hist_path) read_history(hist_path);
    /* E11: install tab-completion generator.
     * editline declares rl_completion_entry_function as Function* (int ret)
     * but actual calling convention is char* — suppress the type mismatch. */
    g_completion_env = env;
#  if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wincompatible-function-pointer-types"
#  elif defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#  endif
    rl_completion_entry_function = tur_completion_generator;
#  if defined(__clang__)
#    pragma clang diagnostic pop
#  elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#  endif
#endif

    Buf multi;
    buf_init(&multi);
    int balance = 0;
    bool in_sweet_form = false; /* 2e: accumulating sweet-exp continuation */

    for (;;) {
        const char *prompt = (multi.len == 0) ? "turmeric> " : "..        ";
        char *line = repl_readline(prompt);

        if (!line) {
            /* EOF (Ctrl-D) */
            if (multi.len > 0) {
                multi.len = 0;
                balance = 0;
                in_sweet_form = false;
                printf("\n");
                continue;
            }
            printf("\n");
            break;
        }

        /* Empty line in multi-line mode:
         * sweet-exp → terminates the form and evaluates;
         * s-expr    → cancels the incomplete expression. */
        if (line[0] == '\0' && multi.len > 0) {
            if (in_sweet_form) {
                /* Blank line terminates the sweet-exp form */
                in_sweet_form = false;
                balance = 0;
                /* fall through to evaluation below */
                buf_putc(&multi, '\0');
                free(line);
                line = NULL;
                goto repl_do_eval;
            }
            printf("(cancelled)\n");
            multi.len = 0;
            balance = 0;
            free(line);
            continue;
        }

        /* Single-line meta-commands (only at the start of an expression) */
        if (multi.len == 0) {
            if (strcmp(line, ":quit") == 0 || strcmp(line, ":q") == 0) {
                free(line);
                break;
            }
            if (strcmp(line, ":help") == 0) {
                print_help();
                free(line);
                continue;
            }
            /* 2d: :reset — clear session and restart with a fresh environment */
            if (strcmp(line, ":reset") == 0) {
                turi_env_free(env);
                env = turi_env_new();
                balance = 0;
                multi.len = 0;
                in_sweet_form = false;
#ifdef TURI_HAVE_EDITLINE
                g_completion_env = env;
#endif
                printf(";; session cleared\n");
                free(line);
                continue;
            }
            if (strncmp(line, ":type", 5) == 0 && (line[5] == ' ' || line[5] == '\0')) {
                const char *expr = (line[5] == ' ') ? line + 6 : "";
                if (*expr) cmd_type(env, expr);
                else printf(":type requires an expression\n");
                free(line);
                continue;
            }
            if (strncmp(line, ":doc", 4) == 0 && (line[4] == ' ' || line[4] == '\0')) {
                const char *sym = (line[4] == ' ') ? line + 5 : "";
                if (*sym) cmd_doc(env, sym);
                else printf(":doc requires a symbol name\n");
                free(line);
                continue;
            }
            if (strncmp(line, ":reload", 7) == 0 && (line[7] == ' ' || line[7] == '\0')) {
                const char *path = (line[7] == ' ') ? line + 8 : "";
                if (*path) cmd_reload(env, path);
                else printf(":reload requires a file path\n");
                free(line);
                continue;
            }
            if (strcmp(line, ":tutorial") == 0) {
                cmd_tutorial_list();
                free(line);
                continue;
            }
            if (strncmp(line, ":tutorial", 9) == 0 && (line[9] == ' ' || line[9] == '\0')) {
                const char *rest = (line[9] == ' ') ? line + 10 : "";
                if (*rest == '\0') {
                    cmd_tutorial_list();
                } else {
                    /* Parse tutorial name and optional step number */
                    char name[256] = {0};
                    int step = 0;
                    int parsed = sscanf(rest, "%255s %d", name, &step);
                    if (parsed >= 1) {
                        cmd_tutorial_start(name, step);
                    } else {
                        printf(":tutorial requires a tutorial name\n");
                    }
                }
                free(line);
                continue;
            }
            if (strcmp(line, ":tutorial-help") == 0) {
                print_tutorial_help();
                free(line);
                continue;
            }
            if (strcmp(line, ":next") == 0) {
                cmd_tutorial_next();
                free(line);
                continue;
            }
            if (strcmp(line, ":prev") == 0) {
                cmd_tutorial_prev();
                free(line);
                continue;
            }
            if (strcmp(line, ":hint") == 0) {
                cmd_tutorial_hint();
                free(line);
                continue;
            }
            if (strcmp(line, ":skip") == 0) {
                cmd_tutorial_skip();
                free(line);
                continue;
            }
            if (strcmp(line, ":quit-tutorial") == 0) {
                cmd_tutorial_quit();
                free(line);
                continue;
            }
            if (strcmp(line, ":tutorial-progress") == 0) {
                cmd_tutorial_progress();
                free(line);
                continue;
            }
            if (strncmp(line, "#lang ", 6) == 0) {
                const char *rest = NULL;
                size_t rest_len  = 0;
                ReaderType rt = detect_lang(line, strlen(line), &rest, &rest_len);
                if (rt == READER_UNKNOWN || rt == (ReaderType)-1) {
                    fprintf(stderr, "unknown #lang: '%s'\n", line + 6);
                } else if (rt != env->reader_type) {
                    env->reader_type    = rt;
                    env->src_acc.len    = 0;   /* accumulated source may be incompatible */
                    env->prior_toplevel = 0;
                    printf("; reader set to %s (session reset)\n", reader_type_name(rt));
                } else {
                    printf("; reader already set to %s\n", reader_type_name(rt));
                }
                free(line);
                continue;
            }
            if (line[0] == ':') {
                printf("unknown meta-command '%s' — try :help\n", line);
                free(line);
                continue;
            }
        }

        /* Accumulate line and update balance. */
        if (line) {
            /* 2e: sweet-exp continuation detection.
             * If in sweet mode and the first token doesn't start with '(',
             * we enter sweet continuation: indented lines continue the form,
             * and a blank line (handled above) terminates it. */
            if (env->reader_type == READER_SWEET && multi.len == 0
                    && line[0] != '(' && line[0] != '\0') {
                in_sweet_form = true;
            }

            if (in_sweet_form) {
                /* Inside a sweet-exp form: keep accumulating unconditionally;
                 * termination is via blank line (handled above). */
                if (multi.len > 0) buf_putc(&multi, '\n');
                buf_puts(&multi, line);
                free(line);
                continue; /* show '..' prompt again */
            }

            balance += paren_balance(line);
            if (multi.len > 0) buf_putc(&multi, '\n');
            buf_puts(&multi, line);
            free(line);
        }

        /* If balanced (or over-closed), evaluate */
        if (balance <= 0) {
            balance = 0;

            if (line) buf_putc(&multi, '\0'); /* already NUL-terminated when line==NULL */
            repl_do_eval:;

            /* RP6: --watch -- check freshness right before eval so the
             * source mutation (which typically happens while readline
             * is blocked on the previous prompt) is picked up against
             * the input the user just submitted, not against the next
             * one. The reload helper prints its own "(reload) rebuilt
             * N exports" summary, satisfying the plan's "one-line
             * summary before the next prompt" requirement. */
            if (watch_mode && env->spice_image
                && !tur_spice_image_is_fresh(env->spice_image)) {
                tur_ffi_reload_spice(env);
            }

            /* Check if we're in tutorial mode and this input matches the current step */
            if (g_tutorial_state && g_tutorial_state->in_tutorial) {
                if (check_tutorial_step(env, multi.data)) {
                    multi.len = 0;
                    continue;
                }
            }

            char     type_tag[64] = {0};
            TuriValue result = turi_eval_typed(env, multi.data,
                                               type_tag, sizeof(type_tag));

            if (turi_is_error(result)) {
                const char *msg = turi_error_message(result);
                if (msg &&
                    strcmp(msg, "parse error") != 0 &&
                    strcmp(msg, "elaboration error") != 0) {
                    if (use_color)
                        fprintf(stderr, COL_ERR "error: %s" COL_RESET "\n", msg);
                    else
                        fprintf(stderr, "error: %s\n", msg);
                }
            } else {
                /* 2h: for type-level forms that return nil, show a sentinel */
                bool type_sentinel = false;
                if (result.tag == TURI_NIL) {
                    char kind[32] = {0}, name[64] = {0};
                    if (detect_type_form(multi.data, kind, sizeof(kind),
                                         name, sizeof(name))) {
                        if (use_color)
                            printf(COL_NIL "=> #<%s %s>" COL_RESET "\n", kind, name);
                        else
                            printf("=> #<%s %s>\n", kind, name);
                        type_sentinel = true;
                    }
                }
                if (!type_sentinel) {
                    /* SI4: three-tier display:
                     *   1. turi_try_show   -- TURI_STRUCT with Show instance
                     *   2. turi_show_result -- TURI_INT heap-pointer (Pair, Cons)
                     *   3. repl_print_value -- default repr */
                    const char *show_str = turi_try_show(env, result);
                    if (!show_str)
                        show_str = turi_show_result(env, result, type_tag);
                    if (show_str) {
                        if (use_color)
                            printf("=> " COL_RESET "%s" COL_RESET "\n", show_str);
                        else
                            printf("=> %s\n", show_str);
                        free((char *)show_str);
                    } else {
                        repl_print_value(result, use_color);
                    }
                    /* 2f: bind _ to last non-nil result */
                    if (result.tag != TURI_NIL)
                        turi_env_set(env, "_", result);
                }
            }
            fflush(stdout);

            multi.len = 0;
        }
    }

    /* Persist history */
#ifdef TURI_HAVE_EDITLINE
    if (hist_path) write_history(hist_path);
#endif
    free(hist_path);

    buf_free(&multi);
    turi_env_free(env);
    return 0;
}
