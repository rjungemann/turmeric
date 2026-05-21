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
    printf("%s", prompt);
    fflush(stdout);
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
    Form **forms = read_all(&arena, &st, &sfile, &nforms);
    if (!forms || diag_had_error()) goto cleanup;

    {
        Expr *prog = elaborate_program(&arena, &st, forms, nforms,
                                       /*stdlib_prefix=*/0, ".",
                                       /*separate_compilation=*/false,
                                       /*tc_env=*/NULL,
                                       /*include_dirs=*/NULL,
                                       /*n_include_dirs=*/0,
                                       /*out_n_fsd=*/NULL);
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

static void cmd_doc(TuriEnv *env, const char *sym) {
    /* Check if the symbol is bound in the env */
    TuriValue v = turi_env_get(env, sym);
    if (!turi_is_error(v)) {
        char repr[256];
        turi_value_repr(repr, sizeof(repr), v);
        printf("%s = %s\n", sym, repr);
        return;
    }

    /* Recognise common builtins with a short description */
    struct { const char *name; const char *doc; } docs[] = {
        {"+",        "(+ a b ...) → add numbers"},
        {"-",        "(- a b ...) → subtract numbers"},
        {"*",        "( * a b ...) → multiply numbers"},
        {"/",        "(/ a b) → divide numbers"},
        {"mod",      "(mod a b) → integer remainder"},
        {"=",        "(= a b) → equality"},
        {"<",        "(< a b) → less-than"},
        {">",        "(> a b) → greater-than"},
        {"<=",       "(<= a b) → less-than-or-equal"},
        {">=",       "(>= a b) → greater-than-or-equal"},
        {"not",      "(not b) → boolean negation"},
        {"and",      "(and a b ...) → short-circuit and"},
        {"or",       "(or a b ...) → short-circuit or"},
        {"println",  "(println x) → print with newline"},
        {"let",      "(let [x v ...] body) → local bindings"},
        {"if",       "(if cond then else) → conditional"},
        {"do",       "(do e ...) → sequence"},
        {"defn",     "(defn name [params] body) → define a function"},
        {"fn",       "(fn [params] body) → anonymous function"},
        {"def",      "(def name value) → top-level binding"},
        {"while",    "(while cond body) → loop"},
        {"set!",     "(set! var value) → mutate a variable"},
        {NULL, NULL}
    };
    for (int i = 0; docs[i].name; i++) {
        if (strcmp(sym, docs[i].name) == 0) {
            printf("%s\n", docs[i].doc);
            return;
        }
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

static void print_help(void) {
    printf(
        "Meta-commands:\n"
        "  :help               show this help\n"
        "  :quit  :q           exit the REPL\n"
        "  :type <expr>        print inferred type without evaluating\n"
        "  :doc  <sym>         print documentation for a symbol or builtin\n"
        "  :reload <file>      evaluate a .tur file into the current session\n"
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

int turi_repl_run(void) {
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
               "\xe2\x96\x84\xe2\x96\x96           \xe2\x96\x98  \n"
               "\xe2\x96\x90 \xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x9b\xe2\x96\x9b\xe2\x96\x8c\xe2\x96\x88\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\n"
               "\xe2\x96\x90 \xe2\x96\x99\xe2\x96\x8c\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\n"
               COL_RESET "\n");
    } else {
        printf("\xe2\x96\x84\xe2\x96\x96           \xe2\x96\x98  \n"
               "\xe2\x96\x90 \xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x9b\xe2\x96\x9b\xe2\x96\x8c\xe2\x96\x88\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\n"
               "\xe2\x96\x90 \xe2\x96\x99\xe2\x96\x8c\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\n"
               "\n");
    }
    printf("Turmeric v" TUR_VERSION "  (type :help for help, :quit to exit)\n");
    fflush(stdout);

    TuriEnv *env = turi_env_new();
    if (!env) {
        fprintf(stderr, "tur repl: failed to create eval environment\n");
        return 1;
    }

    /* Load history */
    char *hist_path = history_path();
#ifdef TURI_HAVE_EDITLINE
    using_history();
    stifle_history(1000);
    if (hist_path) read_history(hist_path);
#endif

    Buf multi;
    buf_init(&multi);
    int balance = 0;

    for (;;) {
        const char *prompt = (multi.len == 0) ? "turmeric> " : "..        ";
        char *line = repl_readline(prompt);

        if (!line) {
            /* EOF (Ctrl-D) */
            if (multi.len > 0) {
                /* Discard incomplete multi-line expression */
                multi.len = 0;
                balance = 0;
                printf("\n");
                continue;
            }
            printf("\n");
            break;
        }

        /* Empty line while in multi-line mode cancels the expression */
        if (line[0] == '\0' && multi.len > 0) {
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
            if (strncmp(line, ":tutorial", 8) == 0 && (line[8] == ' ' || line[8] == '\0')) {
                const char *rest = (line[8] == ' ') ? line + 9 : "";
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

        /* Accumulate line and update balance (use `line` which is NUL-terminated) */
        balance += paren_balance(line);
        if (multi.len > 0) buf_putc(&multi, '\n');
        buf_puts(&multi, line);
        free(line);

        /* If balanced (or over-closed), evaluate */
        if (balance <= 0) {
            balance = 0;

            buf_putc(&multi, '\0');

            /* Check if we're in tutorial mode and this input matches the current step */
            if (g_tutorial_state && g_tutorial_state->in_tutorial) {
                if (check_tutorial_step(env, multi.data)) {
                    /* Tutorial step was completed, result was already printed */
                    multi.len = 0;
                    continue;
                }
            }

            TuriValue result = turi_eval(env, multi.data);

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
                repl_print_value(result, use_color);
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
