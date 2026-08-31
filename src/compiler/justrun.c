/*
 * justrun.c -- tur run: Justfile-compatible task runner.
 *
 * Implements the supported subset from docs/tur-run-plan.md (phases RN0-RN7).
 * Pure C; no Turmeric compiler dependency at runtime.
 *
 * Supported:
 *   - Recipe definitions with shell-command bodies (tab or 2+ space indent)
 *   - Shebang recipes: a body whose first line is `#!` is materialized to a
 *     temp file and exec'd, so it runs under the named interpreter as one
 *     script rather than as independent `sh -c` lines
 *   - Recipe dependencies (chained, deps-with-args)
 *   - Recipe parameters with optional defaults and variadic +PARAM / *PARAM
 *   - Variable assignment: name := "value", export name := "value"
 *   - Interpolation: {{ var }} in recipe bodies and dep args
 *   - Line prefixes: @ (silent) and - (continue on failure), @- and -@
 *   - Settings: set shell, set dotenv-load, set positional-arguments,
 *               set windows-shell (accepted, ignored on POSIX)
 *   - Recipe attributes: [private], [group('name')], [doc("...")],
 *     [confirm] / [confirm("prompt")], [no-cd], [no-exit-message], and the
 *     platform selectors [unix] [linux] [macos] [windows] [openbsd].
 *     Comma-separated lists ([private, group('x')]) are accepted.  An
 *     attribute we do not implement is REFUSED, never silently skipped.
 *   - Built-in functions: env_var, env_var_or_default, os, os_family, arch,
 *     justfile_directory, invocation_directory, uppercase, lowercase,
 *     trim, quote, path_exists, replace, join, error
 *   - Listing: tur run / tur run --list [--json] [--all]; aliases are listed
 *     alongside recipes, [private] and _-prefixed names are hidden, and
 *     [group(...)] recipes are printed in per-group sections
 *   - Variable overrides: --set VAR VALUE (and --set VAR=VALUE)
 *   - The default recipe
 *   - Dotenv loading: .env file from the Justfile's directory
 *   - Unsupported-feature detection with clear error messages
 *
 * Deliberately divergent from upstream `just`:
 *   - Exit 2 means "this Justfile asks for something we do not implement"
 *     (just exits 1); exit 1 is reserved for "the recipe ran and failed".
 *   - We do NOT chdir to the Justfile's directory before running a recipe,
 *     so [no-cd] is accepted but is already the default behavior here.
 *
 * Exit codes:
 *   0   recipe succeeded
 *   1   recipe ran but exited non-zero (propagated)
 *   2   CLI / parse / unsupported-feature error
 *   127 Justfile not found or unreadable
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#elif !defined(_WIN32)
/* Windows is excluded deliberately: MinGW reads _POSIX_C_SOURCE as "hide the
 * Win32 CRT names", which un-declares mkdir/getcwd and hides _finddata_t --
 * which in turn breaks <dirent.h> itself.  glibc needs this macro to EXPOSE
 * those declarations; on Windows it does the exact opposite. */
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "justrun.h"
#include "platform_fs.h"

extern _Bool use_json_output;

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <sys/wait.h>  /* WIFEXITED/WEXITSTATUS; see platform_fs.h on Windows */
#endif
#include <unistd.h>

/* ================================================================== */
/* Limits                                                              */
/* ================================================================== */

#define JR_MAX_PARAMS   16
#define JR_MAX_DEPS     32
#define JR_MAX_LINES   512
#define JR_MAX_RECIPES 256
#define JR_MAX_VARS    128
#define JR_MAX_ALIASES 64
#define JR_MAX_SHELL     8
#define JR_MAX_ISSUES   32

/* ================================================================== */
/* Data structures                                                     */
/* ================================================================== */

typedef struct {
    char *name;
    char *default_val;  /* NULL if no default */
    int   variadic;     /* 1 if +NAME or *NAME */
    int   variadic_req; /* 1 for +NAME (>=1 required), 0 for *NAME */
    /* Option binding from [arg('name', short='v', long='version')].  When
     * opt_long or opt_short is set the parameter is filled from a CLI option
     * instead of by position.  opt_flag_value non-NULL makes it a valueless
     * flag whose presence binds that literal (just's value='true'). */
    char *opt_long;
    char *opt_short;
    char *opt_flag_value;
} JParam;

/* One [arg(...)] attribute, resolved onto a JParam once the recipe header
 * has been parsed (the attribute precedes the header, so the parameter it
 * names does not exist yet when the attribute is read). */
typedef struct {
    char *param;
    char *lng;
    char *shrt;
    char *flag_value;
} JArgSpec;

#define JR_MAX_ARGSPECS 16

typedef struct {
    char  *recipe;
    char **args;
    int    n_args;
} JDep;

typedef struct {
    int   silent;  /* @ prefix: do not echo the command */
    int   cont;    /* - prefix: continue even if shell exits non-zero */
    char *text;    /* interpolation template */
} JLine;

/* Platform selector from [unix] / [windows] / [macos] / [linux] / [openbsd].
 * JR_PLAT_ANY means the recipe carried no platform attribute and runs
 * everywhere; otherwise the value is a bitmask of the platforms named, so
 * `[macos]` and `[unix]` on the same recipe are a union, as in `just`. */
#define JR_PLAT_ANY      0
#define JR_PLAT_LINUX    (1 << 0)
#define JR_PLAT_MACOS    (1 << 1)
#define JR_PLAT_WINDOWS  (1 << 2)
#define JR_PLAT_OPENBSD  (1 << 3)
#define JR_PLAT_UNIX     (JR_PLAT_LINUX | JR_PLAT_MACOS | JR_PLAT_OPENBSD)

#if defined(__APPLE__)
#  define JR_HOST_OS_NAME "macos"
#elif defined(__linux__)
#  define JR_HOST_OS_NAME "linux"
#elif defined(_WIN32)
#  define JR_HOST_OS_NAME "windows"
#elif defined(__OpenBSD__)
#  define JR_HOST_OS_NAME "openbsd"
#else
#  define JR_HOST_OS_NAME "unknown"
#endif

/* Attributes attached to a recipe.  Accumulated across consecutive `[...]`
 * lines above a recipe header, then copied onto the recipe. */
typedef struct {
    int   hidden;           /* [private] */
    int   no_cd;            /* [no-cd] */
    int   no_exit_message;  /* [no-exit-message] */
    int   confirm;          /* [confirm] or [confirm("...")] */
    char *confirm_msg;      /* the custom prompt, or NULL for the default */
    char *group;            /* [group('name')], or NULL */
    char *doc_attr;         /* [doc("...")], overrides a `#` doc comment */
    int   platform;         /* JR_PLAT_* bitmask; JR_PLAT_ANY = unrestricted */
    JArgSpec argspecs[JR_MAX_ARGSPECS];
    int      n_argspecs;
} JAttrs;

typedef struct {
    char   *name;
    char   *doc;   /* accumulated doc comment, or NULL */
    int     hidden; /* [private] attribute or a leading '_': omit from --list */
    JAttrs  attrs;
    JParam  params[JR_MAX_PARAMS];
    int     n_params;
    JDep    deps[JR_MAX_DEPS];
    int     n_deps;
    JLine   lines[JR_MAX_LINES];
    int     n_lines;
} JRecipe;

typedef struct {
    char *name;
    char *value;
    int   exported;
} JVar;

typedef struct {
    char *shell[JR_MAX_SHELL];
    int   n_shell;
    int   dotenv_load;
    int   positional_arguments;
} JSettings;

typedef struct {
    char *name;
    char *target;
} JAlias;

typedef struct {
    JRecipe  recipes[JR_MAX_RECIPES];
    int      n_recipes;
    JVar     vars[JR_MAX_VARS];
    int      n_vars;
    JAlias   aliases[JR_MAX_ALIASES];
    int      n_aliases;
    JSettings settings;
    char    *justfile_dir;
    /* Unsupported-feature diagnostics collected during the parse. Listing
     * tolerates them (and reports them on stderr) so shell completion still
     * gets candidates; executing a recipe is still a hard error. */
    char    *issues[JR_MAX_ISSUES];
    int      n_issues;
} JFile;

/* ================================================================== */
/* Memory helpers                                                      */
/* ================================================================== */

static char *jr_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char  *p = (char *)malloc(n + 1);
    if (!p) { fprintf(stderr, "tur run: out of memory\n"); exit(2); }
    memcpy(p, s, n + 1);
    return p;
}

static char *jr_strndup(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) { fprintf(stderr, "tur run: out of memory\n"); exit(2); }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ================================================================== */
/* String utilities                                                    */
/* ================================================================== */

static const char *jr_ltrim(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static char *jr_trim(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\n' || s[n-1] == '\r'))
        n--;
    return jr_strndup(s, n);
}

static int jr_starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* ================================================================== */
/* Value parser                                                        */
/* ================================================================== */

/* Parse a Justfile value: double-quoted, single-quoted, array, or bare word.
 * Returns a heap-allocated string; sets *end to the first unparsed byte. */
static char *parse_value(const char *p, const char **end) {
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '"') {
        const char *start = p + 1;
        const char *q = start;
        while (*q && *q != '"' && *q != '\n') q++;
        if (end) *end = (*q == '"') ? q + 1 : q;
        return jr_strndup(start, (size_t)(q - start));
    }
    if (*p == '\'') {
        const char *start = p + 1;
        const char *q = start;
        while (*q && *q != '\'' && *q != '\n') q++;
        if (end) *end = (*q == '\'') ? q + 1 : q;
        return jr_strndup(start, (size_t)(q - start));
    }
    if (*p == '[') {
        /* Array literal: ["sh", "-c"] */
        const char *start = p;
        int depth = 0;
        const char *q = p;
        while (*q) {
            if (*q == '[') depth++;
            else if (*q == ']') { if (--depth == 0) { q++; break; } }
            q++;
        }
        if (end) *end = q;
        return jr_strndup(start, (size_t)(q - start));
    }
    /* Bare word */
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '#')
        p++;
    if (end) *end = p;
    return jr_strndup(start, (size_t)(p - start));
}

/* Parse a shell array literal: ["sh", "-c"] -> argv-style array. */
static char **parse_shell_array(const char *s, int *n_out) {
    *n_out = 0;
    if (!s || *s != '[') {
        /* Default */
        char **arr = (char **)malloc(2 * sizeof(char *));
        if (!arr) return NULL;
        arr[0] = jr_strdup("sh");
        arr[1] = jr_strdup("-c");
        *n_out = 2;
        return arr;
    }
    int   cap = 4;
    char **arr = (char **)malloc((size_t)cap * sizeof(char *));
    if (!arr) return NULL;
    const char *p = s + 1;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == ']' || !*p) break;
        const char *end;
        char *val = parse_value(p, &end);
        p = end;
        if (*n_out >= cap) {
            cap *= 2;
            arr = (char **)realloc(arr, (size_t)cap * sizeof(char *));
            if (!arr) return NULL;
        }
        arr[(*n_out)++] = val;
    }
    return arr;
}

/* ================================================================== */
/* Dotenv loader                                                       */
/* ================================================================== */

static void load_dotenv(const char *dir) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/.env", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        if (jr_starts_with(p, "export ")) p += 7;
        while (*p == ' ' || *p == '\t') p++;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        char *key = jr_trim(jr_strndup(p, (size_t)(eq - p)));
        char *vstart = eq + 1;
        char *nl = strchr(vstart, '\n');
        if (nl) *nl = '\0';
        char *val = jr_trim(vstart);
        /* Strip surrounding quotes */
        size_t vlen = strlen(val);
        if (vlen >= 2 && ((val[0] == '"' && val[vlen-1] == '"') ||
                          (val[0] == '\'' && val[vlen-1] == '\''))) {
            char *stripped = jr_strndup(val + 1, vlen - 2);
            free(val);
            val = stripped;
        }
        setenv(key, val, 0); /* 0 = don't overwrite existing */
        free(key);
        free(val);
    }
    fclose(f);
}

/* ================================================================== */
/* Unsupported-feature detection                                       */
/* ================================================================== */

/* Format an unsupported-feature message into a fresh heap string. */
static char *jr_issuef(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return jr_strdup(buf);
}

/* ------------------------------------------------------------------ */
/* Attribute lines                                                     */
/* ------------------------------------------------------------------ */

/* Release every heap string an attribute set owns, and zero it. */
static void jattrs_free(JAttrs *at) {
    free(at->confirm_msg);
    free(at->group);
    free(at->doc_attr);
    for (int i = 0; i < at->n_argspecs; i++) {
        free(at->argspecs[i].param);
        free(at->argspecs[i].lng);
        free(at->argspecs[i].shrt);
        free(at->argspecs[i].flag_value);
    }
    memset(at, 0, sizeof(*at));
}

/* Is this line an attribute line -- `[` at the start, and no `:=` before the
 * bracket (which would make it an array-valued assignment RHS instead)?  We
 * only need to distinguish the two forms; the caller has already left-trimmed. */
static int jr_is_attr_line(const char *p) {
    return *p == '[' && p[1] != '\0' && p[1] != ' ' && p[1] != '\n' &&
           strstr(p, ":=") == NULL;
}

/* Read a single-or-double-quoted string literal starting at *pp (which must
 * point at the quote).  Returns a fresh string and advances *pp past the
 * closing quote, or NULL if unterminated. */
static char *jr_attr_string(const char **pp) {
    const char *p = *pp;
    char quote = *p;
    if (quote != '\'' && quote != '"') return NULL;
    p++;
    const char *start = p;
    while (*p && *p != quote) p++;
    if (*p != quote) return NULL;
    char *out = jr_strndup(start, (size_t)(p - start));
    *pp = p + 1;
    return out;
}

/* Parse one attribute line into `at`.  Handles `[a]`, `[a(x)]`, and the
 * comma-separated form `[a, b('c')]`.
 *
 * Matching is on the attribute NAME -- the text up to `(` or `]` -- so an
 * attribute that carries an argument list is recognized rather than falling
 * through to be silently skipped.  Anything not recognized is REFUSED, not
 * ignored: an unknown attribute means the Justfile is asking for behavior we
 * do not implement, and running the recipe anyway would silently drop a
 * constraint the author wrote down (the `[confirm("...")]` case, where
 * ignoring the attribute skips a prompt guarding a destructive command).
 *
 * Returns NULL on success, else a heap-allocated diagnostic. */
static char *parse_attr_line(const char *line, int lineno, const char *path,
                             JAttrs *at) {
    const char *p = jr_ltrim(line);
    if (*p != '[') return NULL;
    p++;

    for (;;) {
        while (*p == ' ' || *p == '\t') p++;

        const char *nstart = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_')) p++;
        if (p == nstart) {
            return jr_issuef("%s:%d: malformed recipe attribute", path, lineno);
        }
        char name[64] = {0};
        size_t nlen = (size_t)(p - nstart);
        if (nlen >= sizeof(name)) {
            return jr_issuef("%s:%d: recipe attribute name too long", path, lineno);
        }
        memcpy(name, nstart, nlen);

        while (*p == ' ' || *p == '\t') p++;

        /* [arg('param', short='v', long='version', value='true')] needs the
         * whole keyword-argument list, not just the first string, so it is
         * parsed here rather than falling into the single-argument path. */
        if (strcmp(name, "arg") == 0) {
            if (*p != '(') {
                return jr_issuef("%s:%d: [arg] requires an argument list, e.g. "
                                 "[arg('name', long='name')]", path, lineno);
            }
            if (at->n_argspecs >= JR_MAX_ARGSPECS) {
                return jr_issuef("%s:%d: too many [arg(...)] attributes",
                                 path, lineno);
            }
            p++;
            JArgSpec *spec = &at->argspecs[at->n_argspecs];
            memset(spec, 0, sizeof(*spec));
            int first = 1;
            for (;;) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                if (*p == ')') { p++; break; }
                if (*p == '\0') {
                    return jr_issuef("%s:%d: unterminated [arg(...)]",
                                     path, lineno);
                }
                if (first && (*p == '\'' || *p == '"')) {
                    spec->param = jr_attr_string(&p);
                    if (!spec->param) {
                        return jr_issuef("%s:%d: unterminated string in [arg(...)]",
                                         path, lineno);
                    }
                    first = 0;
                    continue;
                }
                first = 0;
                /* key = 'value' */
                const char *kstart = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
                char key[32] = {0};
                size_t klen = (size_t)(p - kstart);
                if (klen == 0 || klen >= sizeof(key)) {
                    return jr_issuef("%s:%d: malformed key in [arg(...)]",
                                     path, lineno);
                }
                memcpy(key, kstart, klen);
                while (*p == ' ' || *p == '\t') p++;
                if (*p != '=') {
                    return jr_issuef("%s:%d: expected '=' after '%s' in [arg(...)]",
                                     path, lineno, key);
                }
                p++;
                while (*p == ' ' || *p == '\t') p++;
                char *val = jr_attr_string(&p);
                if (!val) {
                    return jr_issuef("%s:%d: expected a quoted value for '%s' "
                                     "in [arg(...)]", path, lineno, key);
                }
                if      (strcmp(key, "long")  == 0) { free(spec->lng);  spec->lng  = val; }
                else if (strcmp(key, "short") == 0) { free(spec->shrt); spec->shrt = val; }
                else if (strcmp(key, "value") == 0) { free(spec->flag_value); spec->flag_value = val; }
                else {
                    free(val);
                    return jr_issuef("%s:%d: unknown key '%s' in [arg(...)] "
                                     "(expected long, short, or value)",
                                     path, lineno, key);
                }
            }
            if (!spec->param) {
                return jr_issuef("%s:%d: [arg(...)] needs a parameter name as "
                                 "its first argument", path, lineno);
            }
            /* Default the long option to the parameter name, as just does. */
            if (!spec->lng && !spec->shrt) spec->lng = jr_strdup(spec->param);
            at->n_argspecs++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ',') { p++; continue; }
            break;
        }

        /* Optional argument list. We keep only the first string argument,
         * which is all any attribute we support needs. */
        char *arg = NULL;
        if (*p == '(') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\'' || *p == '"') {
                arg = jr_attr_string(&p);
                if (!arg) {
                    return jr_issuef("%s:%d: unterminated string in attribute "
                                     "[%s(...)]", path, lineno, name);
                }
            }
            /* Skip to the matching close paren, ignoring any further args. */
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                p++;
            }
            if (depth != 0) {
                free(arg);
                return jr_issuef("%s:%d: unterminated argument list in attribute "
                                 "[%s(...)]", path, lineno, name);
            }
        }

        if (strcmp(name, "private") == 0) {
            at->hidden = 1;
        } else if (strcmp(name, "no-cd") == 0) {
            at->no_cd = 1;
        } else if (strcmp(name, "no-exit-message") == 0) {
            at->no_exit_message = 1;
        } else if (strcmp(name, "confirm") == 0) {
            at->confirm = 1;
            free(at->confirm_msg);
            at->confirm_msg = arg;  /* may be NULL: bare [confirm] */
            arg = NULL;
        } else if (strcmp(name, "group") == 0) {
            free(at->group);
            at->group = arg;
            arg = NULL;
        } else if (strcmp(name, "doc") == 0) {
            free(at->doc_attr);
            at->doc_attr = arg;
            arg = NULL;
        } else if (strcmp(name, "unix") == 0) {
            at->platform |= JR_PLAT_UNIX;
        } else if (strcmp(name, "linux") == 0) {
            at->platform |= JR_PLAT_LINUX;
        } else if (strcmp(name, "macos") == 0) {
            at->platform |= JR_PLAT_MACOS;
        } else if (strcmp(name, "windows") == 0) {
            at->platform |= JR_PLAT_WINDOWS;
        } else if (strcmp(name, "openbsd") == 0) {
            at->platform |= JR_PLAT_OPENBSD;
        } else {
            free(arg);
            return jr_issuef(
                "unsupported Justfile feature at %s:%d: "
                "recipe attribute [%s]\n"
                "        Install `just` (https://just.systems) to run "
                "this recipe, or remove the [%s] attribute if the "
                "recipe is portable.",
                path, lineno, name, name);
        }
        free(arg);

        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') { p++; continue; }
        break;
    }
    return NULL;
}

/* Does `platform` (a JR_PLAT_* bitmask) include the host we are running on? */
static int jr_platform_matches(int platform) {
    if (platform == JR_PLAT_ANY) return 1;
#if defined(__APPLE__)
    return (platform & JR_PLAT_MACOS) != 0;
#elif defined(__linux__)
    return (platform & JR_PLAT_LINUX) != 0;
#elif defined(_WIN32)
    return (platform & JR_PLAT_WINDOWS) != 0;
#elif defined(__OpenBSD__)
    return (platform & JR_PLAT_OPENBSD) != 0;
#else
    return (platform & JR_PLAT_UNIX) != 0;
#endif
}

/* Backtick command substitution inside `{{ ... }}` interpolation.
 *
 * Without this the interpolation evaluator finds no variable named "`cmd`"
 * and yields the empty string, so the recipe runs with a value silently
 * missing.  Refuse it the same way the assignment path does -- an honest
 * error beats a plausible-looking command with a hole in it.
 *
 * Called for recipe BODY lines as well as top-level lines: body lines are
 * consumed by the parse loop before check_unsupported ever sees them, and a
 * body line is where an interpolated backtick actually shows up. */
static char *check_interp_backtick(const char *p, int lineno, const char *path) {
    for (const char *q = p; (q = strstr(q, "{{")) != NULL; ) {
        const char *end = strstr(q + 2, "}}");
        if (!end) break;
        if (memchr(q + 2, '`', (size_t)(end - (q + 2))) != NULL) {
            return jr_issuef(
                "unsupported Justfile feature at %s:%d: "
                "backtick command substitution in interpolation\n"
                "        Install `just` (https://just.systems) for this "
                "feature.",
                path, lineno);
        }
        q = end + 2;
    }
    return NULL;
}

/* Returns NULL when the line uses nothing unsupported, else a heap-allocated
 * message describing what is unsupported (caller owns it).  Attribute lines
 * are handled by parse_attr_line before this is reached. */
static char *check_unsupported(const char *line, int lineno, const char *path) {
    const char *p = jr_ltrim(line);

    /* Module / import directives */
    if (jr_starts_with(p, "mod ") || jr_starts_with(p, "import '") ||
        jr_starts_with(p, "import \"")) {
        return jr_issuef(
            "unsupported Justfile feature at %s:%d: module/import directive\n"
            "        Install `just` (https://just.systems) to use modules.",
            path, lineno);
    }

    /* Backtick command substitution in assignment RHS */
    {
        const char *assign = strstr(p, ":=");
        if (assign) {
            const char *rhs = assign + 2;
            while (*rhs == ' ' || *rhs == '\t') rhs++;
            if (*rhs == '`') {
                return jr_issuef(
                    "unsupported Justfile feature at %s:%d: "
                    "backtick command substitution in assignment\n"
                    "        Install `just` (https://just.systems) for this "
                    "feature.",
                    path, lineno);
            }
        }
    }

    {
        char *issue = check_interp_backtick(p, lineno, path);
        if (issue) return issue;
    }

    /* Top-level `if` conditionals are only valid inside assignment RHS (which
     * the RHS evaluator handles). A bare top-level `if` statement is not a
     * just construct, so fall through and let the normal parser flag it as a
     * junk line. */

    /* Alias is handled directly in the parse loop now (see parse_justfile). */

    return NULL;
}

/* ================================================================== */
/* Recipe header parser                                                */
/* ================================================================== */

/* Returns 1 on success, 0 on failure (not a recipe header). */
static int parse_recipe_header(const char *line, JRecipe *r) {
    const char *p = line;

    /* Name: identifier characters at column 0 */
    const char *name_start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != ':' && *p != '\n') p++;
    if (p == name_start || *p == '\n' || *p == '\0') return 0;
    /* Name must not contain shell-special chars that indicate it isn't a recipe */
    for (const char *q = name_start; q < p; q++) {
        if (*q == '=' || *q == '(' || *q == ')') return 0;
    }
    r->name = jr_strndup(name_start, (size_t)(p - name_start));

    while (*p == ' ' || *p == '\t') p++;

    /* Parameters (before the colon) */
    r->n_params = 0;
    while (*p && *p != ':') {
        if (*p == '\n' || *p == '\r') break;

        int variadic = 0, variadic_req = 0;
        if (*p == '+') { variadic = 1; variadic_req = 1; p++; }
        else if (*p == '*') { variadic = 1; variadic_req = 0; p++; }

        const char *pname_start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ':' && *p != '=' && *p != '\n') p++;
        if (p == pname_start) break;

        JParam *param = NULL;
        if (r->n_params < JR_MAX_PARAMS) {
            param = &r->params[r->n_params++];
            param->name        = jr_strndup(pname_start, (size_t)(p - pname_start));
            param->default_val = NULL;
            param->variadic    = variadic;
            param->variadic_req= variadic_req;
        }

        if (*p == '=') {
            p++;
            const char *end;
            char *val = parse_value(p, &end);
            if (param) param->default_val = val; else free(val);
            p = end;
        }
        while (*p == ' ' || *p == '\t') p++;
    }

    if (*p != ':') { free(r->name); r->name = NULL; return 0; }
    p++;

    /* Dependencies */
    r->n_deps = 0;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != '\n' && *p != '\r' && *p != '#') {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r' || *p == '#') break;

        JDep *dep = (r->n_deps < JR_MAX_DEPS) ? &r->deps[r->n_deps++] : NULL;

        if (*p == '(') {
            /* Dep-with-args: (recipe arg1 arg2) */
            p++;
            const char *rstart = p;
            while (*p && *p != ' ' && *p != '\t' && *p != ')' && *p != '\n') p++;
            if (dep) {
                dep->recipe = jr_strndup(rstart, (size_t)(p - rstart));
                dep->n_args = 0;
                dep->args   = NULL;
            }
            int   arg_cap = 4;
            char **args  = (char **)malloc((size_t)arg_cap * sizeof(char *));
            int    n_args= 0;
            while (*p && *p != ')' && *p != '\n') {
                while (*p == ' ' || *p == '\t') p++;
                if (*p == ')' || !*p || *p == '\n') break;
                const char *end;
                char *arg = parse_value(p, &end);
                p = end;
                if (n_args >= arg_cap) {
                    arg_cap *= 2;
                    args = (char **)realloc(args, (size_t)arg_cap * sizeof(char *));
                }
                args[n_args++] = arg;
            }
            if (*p == ')') p++;
            if (dep) { dep->args = args; dep->n_args = n_args; }
            else { for (int i = 0; i < n_args; i++) free(args[i]); free(args); }
        } else {
            /* Simple dep */
            const char *dstart = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '#')
                p++;
            if (dep) {
                dep->recipe = jr_strndup(dstart, (size_t)(p - dstart));
                dep->args   = NULL;
                dep->n_args = 0;
            }
        }
        while (*p == ' ' || *p == '\t') p++;
    }

    return 1;
}

/* Check if a line is a body line (tab-indented or 2+ space-indented). */
static int is_body_line(const char *line) {
    if (line[0] == '\t') return 1;
    if (line[0] == ' ' && line[1] == ' ') return 1;
    return 0;
}

/* Parse the @ / - prefix of a body line and return pointer to actual command. */
static void parse_body_prefix(const char *p, JLine *jline, const char **cmd_start) {
    while (*p == ' ' || *p == '\t') p++;
    jline->silent = 0;
    jline->cont   = 0;
    for (;;) {
        if (*p == '@') { jline->silent = 1; p++; }
        else if (*p == '-') { jline->cont = 1; p++; }
        else break;
    }
    *cmd_start = p;
}

/* ================================================================== */
/* RHS scanner + expression evaluator                                  */
/* ================================================================== */

/* Forward decl -- implementation lives after eval_builtin.
 *
 * Returns NULL both for "no such builtin" and for "the builtin failed"
 * (env_var on an unset variable, error(...)).  `*failed` distinguishes them:
 * it is set to 1 only in the second case.  Without that split, a failing
 * builtin was reported as an unknown one AND its empty result was spliced
 * into the command, which then ran. */
static char *eval_builtin(const char *name, const char **args, int n_args,
                            const JFile *jf, int *failed);

/* Balanced RHS scanner: reads from `p` until end-of-logical-line, balancing
 * (), {}, [] and skipping over "..." / '...' literals. Honors '#' as an
 * end-of-line comment marker only at bracket depth 0 outside strings.
 * Returns malloc'd trimmed text; sets *end to the first unconsumed byte
 * (the '#', '\0', or '\n'). */
static char *parse_rhs_expr_text(const char *p, const char **end) {
    while (*p == ' ' || *p == '\t') p++;
    const char *body_start = p;
    int paren = 0, brace = 0, brack = 0;
    while (*p) {
        char c = *p;
        if (c == '"' || c == '\'') {
            char q = c;
            p++;
            while (*p && *p != q && *p != '\n') {
                if (*p == '\\' && p[1]) p += 2;
                else p++;
            }
            if (*p == q) p++;
            continue;
        }
        if (c == '\n' || c == '\r' || c == '\0') break;
        if (c == '#' && paren == 0 && brace == 0 && brack == 0) break;
        if (c == '(') paren++;
        else if (c == ')') paren--;
        else if (c == '{') brace++;
        else if (c == '}') brace--;
        else if (c == '[') brack++;
        else if (c == ']') brack--;
        p++;
    }
    const char *tail = p;
    while (tail > body_start && (tail[-1] == ' ' || tail[-1] == '\t')) tail--;
    if (end) *end = p;
    return jr_strndup(body_start, (size_t)(tail - body_start));
}

/* Recursive-descent evaluator state. */
typedef struct {
    const char *p;
    const char *path;
    int         lineno;
    JFile      *jf;
    int         error;
} REval;

static void re_skip_ws(REval *r) {
    while (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')
        r->p++;
}

static void re_error(REval *r, const char *msg) {
    if (!r->error) {
        fprintf(stderr, "tur run: %s:%d: %s\n",
                r->path ? r->path : "<justfile>", r->lineno, msg);
        r->error = 1;
    }
}

static char *re_expr(REval *r);  /* forward */

static char *re_string_literal(REval *r) {
    char q = *r->p++;
    size_t cap = 32;
    char *buf = (char *)malloc(cap);
    size_t bl = 0;
    while (*r->p && *r->p != q && *r->p != '\n') {
        if (bl + 2 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        if (*r->p == '\\' && r->p[1]) {
            char c = r->p[1];
            switch (c) {
                case 'n':  buf[bl++] = '\n'; break;
                case 't':  buf[bl++] = '\t'; break;
                case 'r':  buf[bl++] = '\r'; break;
                case '\\': buf[bl++] = '\\'; break;
                case '"':  buf[bl++] = '"';  break;
                case '\'': buf[bl++] = '\''; break;
                default:   buf[bl++] = c;    break;
            }
            r->p += 2;
        } else {
            buf[bl++] = *r->p++;
        }
    }
    if (*r->p == q) r->p++;
    else re_error(r, "unterminated string literal");
    buf[bl] = '\0';
    return buf;
}

static char *re_primary(REval *r) {
    re_skip_ws(r);
    if (*r->p == '"' || *r->p == '\'') return re_string_literal(r);
    if (*r->p == '(') {
        r->p++;
        char *v = re_expr(r);
        re_skip_ws(r);
        if (*r->p == ')') r->p++;
        else re_error(r, "expected ')'");
        return v;
    }
    /* 'if' EXPR COMPOP EXPR '{' EXPR '}' 'else' '{' EXPR '}' */
    if (strncmp(r->p, "if", 2) == 0 &&
        (r->p[2] == ' ' || r->p[2] == '\t' || r->p[2] == '(')) {
        r->p += 2;
        char *lhs = re_expr(r);
        re_skip_ws(r);
        int neq;
        if (strncmp(r->p, "==", 2) == 0) { neq = 0; r->p += 2; }
        else if (strncmp(r->p, "!=", 2) == 0) { neq = 1; r->p += 2; }
        else {
            re_error(r, "expected '==' or '!=' in conditional");
            free(lhs);
            return jr_strdup("");
        }
        char *rhs = re_expr(r);
        re_skip_ws(r);
        if (*r->p != '{') { re_error(r, "expected '{' after conditional"); }
        else r->p++;
        char *then_v = re_expr(r);
        re_skip_ws(r);
        if (*r->p == '}') r->p++;
        else re_error(r, "expected '}' after then-branch");
        re_skip_ws(r);
        if (strncmp(r->p, "else", 4) != 0 ||
            !(r->p[4] == ' ' || r->p[4] == '\t' || r->p[4] == '{')) {
            re_error(r, "expected 'else' in conditional");
        } else {
            r->p += 4;
        }
        re_skip_ws(r);
        if (*r->p != '{') { re_error(r, "expected '{' after else"); }
        else r->p++;
        char *else_v = re_expr(r);
        re_skip_ws(r);
        if (*r->p == '}') r->p++;
        else re_error(r, "expected '}' after else-branch");
        int cond = (strcmp(lhs, rhs) == 0);
        if (neq) cond = !cond;
        char *chosen = cond ? then_v : else_v;
        char *other  = cond ? else_v : then_v;
        free(lhs); free(rhs); free(other);
        return chosen;
    }
    /* identifier: function call or variable lookup */
    if (isalpha((unsigned char)*r->p) || *r->p == '_') {
        const char *ns = r->p;
        while (*r->p && (isalnum((unsigned char)*r->p) || *r->p == '_'))
            r->p++;
        char *name = jr_strndup(ns, (size_t)(r->p - ns));
        re_skip_ws(r);
        if (*r->p == '(') {
            r->p++;
            char *args[8];
            int   n_args = 0;
            re_skip_ws(r);
            if (*r->p != ')') {
                while (1) {
                    if (n_args >= 8) {
                        re_error(r, "too many arguments (max 8)");
                        break;
                    }
                    args[n_args++] = re_expr(r);
                    re_skip_ws(r);
                    if (*r->p == ',') { r->p++; re_skip_ws(r); continue; }
                    break;
                }
            }
            re_skip_ws(r);
            if (*r->p == ')') r->p++;
            else re_error(r, "expected ')' in function call");
            int   bfailed = 0;
            char *result  = eval_builtin(name, (const char **)args, n_args,
                                         r->jf, &bfailed);
            for (int i = 0; i < n_args; i++) free(args[i]);
            if (!result) {
                char msg[256];
                snprintf(msg, sizeof(msg), "%s built-in function '%s'",
                         bfailed ? "failed call to" : "unknown", name);
                re_error(r, msg);
                result = jr_strdup("");
            }
            free(name);
            return result;
        }
        /* Variable lookup in already-parsed assignments. */
        for (int i = 0; i < r->jf->n_vars; i++) {
            if (strcmp(r->jf->vars[i].name, name) == 0) {
                char *v = jr_strdup(r->jf->vars[i].value);
                free(name);
                return v;
            }
        }
        /* Fall through to environment. */
        const char *ev = getenv(name);
        if (ev) {
            char *v = jr_strdup(ev);
            free(name);
            return v;
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "unknown variable '%s'", name);
        re_error(r, msg);
        free(name);
        return jr_strdup("");
    }
    re_error(r, "expected expression");
    return jr_strdup("");
}

/* Concat with '/' -- join with a single slash unless one side already
 * has one at the boundary. Concat with '+' -- plain string append. */
static char *re_concat(char *a, char op, char *b) {
    size_t la = strlen(a), lb = strlen(b);
    size_t skip_b = 0;
    int    need_slash = 0;
    if (op == '/') {
        int a_has = (la > 0 && a[la - 1] == '/');
        int b_has = (lb > 0 && b[0] == '/');
        if (a_has && b_has) skip_b = 1;
        else if (!a_has && !b_has) need_slash = 1;
    }
    size_t out_len = la + (lb - skip_b) + (size_t)need_slash;
    char  *out = (char *)malloc(out_len + 1);
    memcpy(out, a, la);
    size_t off = la;
    if (need_slash) out[off++] = '/';
    memcpy(out + off, b + skip_b, lb - skip_b);
    out[out_len] = '\0';
    free(a);
    free(b);
    return out;
}

static char *re_expr(REval *r) {
    char *left = re_primary(r);
    while (!r->error) {
        re_skip_ws(r);
        char op;
        if (*r->p == '/') op = '/';
        else if (*r->p == '+') op = '+';
        else break;
        r->p++;
        char *right = re_primary(r);
        left = re_concat(left, op, right);
    }
    return left;
}

/* Parse+evaluate the RHS of an assignment. Returns malloc'd value on
 * success, NULL on error (message already printed). */
static char *eval_rhs(const char *text, JFile *jf, const char *path,
                        int lineno) {
    REval r;
    r.p      = text;
    r.path   = path;
    r.lineno = lineno;
    r.jf     = jf;
    r.error  = 0;
    char *v = re_expr(&r);
    re_skip_ws(&r);
    if (!r.error && *r.p) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "unexpected token in assignment RHS starting at '%.32s'", r.p);
        re_error(&r, msg);
    }
    if (r.error) { free(v); return NULL; }
    return v;
}

/* ================================================================== */
/* Justfile parser                                                     */
/* ================================================================== */

static int parse_justfile(const char *text, const char *path, JFile *jf) {
    const char *p = text;
    int lineno    = 0;

    char    *pending_doc     = NULL;
    JRecipe *cur_recipe      = NULL;
    int      pending_private = 0;
    JAttrs   pending_attrs;
    memset(&pending_attrs, 0, sizeof(pending_attrs));

    while (*p) {
        lineno++;
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        size_t line_len = (size_t)(eol - p);

        char *line = jr_strndup(p, line_len);
        /* Strip trailing \r */
        if (line_len > 0 && line[line_len - 1] == '\r') line[line_len - 1] = '\0';

        p = (*eol == '\n') ? eol + 1 : eol;

        /* ---- Body line (must follow a recipe header) ---- */
        if (is_body_line(line) && cur_recipe) {
            const char *cmd_start;
            JLine jline;
            parse_body_prefix(line, &jline, &cmd_start);
            /* A body line never reaches check_unsupported, so the constructs
             * that only appear in a body get checked here. */
            char *issue = check_interp_backtick(cmd_start, lineno, path);
            if (issue) {
                if (jf->n_issues < JR_MAX_ISSUES) jf->issues[jf->n_issues++] = issue;
                else free(issue);
            }
            jline.text = jr_strdup(cmd_start);
            if (cur_recipe->n_lines < JR_MAX_LINES)
                cur_recipe->lines[cur_recipe->n_lines++] = jline;
            free(line);
            continue;
        }

        /* Any non-body non-blank line ends the current recipe's body. */
        cur_recipe = NULL;

        /* ---- Blank line ---- */
        const char *t = jr_ltrim(line);
        if (*t == '\0') {
            free(pending_doc);
            pending_doc     = NULL;
            pending_private = 0;
            free(line);
            continue;
        }

        /* ---- Alias: `alias NAME := TARGET` ---- */
        if (jr_starts_with(t, "alias ") && strstr(t, ":=")) {
            const char *q = t + 6;
            while (*q == ' ' || *q == '\t') q++;
            const char *name_start = q;
            while (*q && *q != ' ' && *q != '\t' && *q != ':' && *q != '\n') q++;
            char *aname = jr_strndup(name_start, (size_t)(q - name_start));
            const char *eq = strstr(q, ":=");
            if (aname && eq) {
                const char *rhs = eq + 2;
                while (*rhs == ' ' || *rhs == '\t') rhs++;
                const char *tgt_start = rhs;
                while (*rhs && *rhs != ' ' && *rhs != '\t' &&
                       *rhs != '\n' && *rhs != '\r' && *rhs != '#') rhs++;
                char *target = jr_strndup(tgt_start, (size_t)(rhs - tgt_start));
                if (target && *target && jf->n_aliases < JR_MAX_ALIASES) {
                    jf->aliases[jf->n_aliases].name   = aname;
                    jf->aliases[jf->n_aliases].target = target;
                    jf->n_aliases++;
                } else {
                    free(aname);
                    free(target);
                }
            } else {
                free(aname);
            }
            free(pending_doc);
            pending_doc = NULL;
            free(line);
            continue;
        }

        /* ---- Recipe attribute line ----
         * Accumulates into pending_attrs, which the next recipe header claims.
         * pending_doc deliberately survives: a doc comment above an attribute
         * still belongs to the recipe below it. */
        if (jr_is_attr_line(t)) {
            char *issue = parse_attr_line(line, lineno, path, &pending_attrs);
            if (issue) {
                if (jf->n_issues < JR_MAX_ISSUES) jf->issues[jf->n_issues++] = issue;
                else free(issue);
            }
            if (pending_attrs.hidden) pending_private = 1;
            free(line);
            continue;
        }

        /* ---- Unsupported feature check ----
         * Never fatal here: the issue is recorded and the line skipped, so a
         * listing (and therefore shell completion) still sees every recipe the
         * parser CAN handle.  cmd_justrun turns a recorded issue into a hard
         * error when a recipe is actually being executed. */
        {
            char *issue = check_unsupported(line, lineno, path);
            if (issue) {
                if (jf->n_issues < JR_MAX_ISSUES) jf->issues[jf->n_issues++] = issue;
                else free(issue);
                free(line);
                continue;
            }
        }

        /* ---- Comment / doc accumulation ---- */
        if (*t == '#') {
            const char *comment = t + 1;
            if (*comment == ' ') comment++;
            char *stripped = jr_trim(comment);
            if (pending_doc) {
                size_t old_len = strlen(pending_doc);
                size_t new_len = strlen(stripped);
                char *combined = (char *)malloc(old_len + new_len + 2);
                memcpy(combined, pending_doc, old_len);
                combined[old_len] = '\n';
                memcpy(combined + old_len + 1, stripped, new_len + 1);
                free(pending_doc);
                free(stripped);
                pending_doc = combined;
            } else {
                pending_doc = stripped;
            }
            free(line);
            continue;
        }

        /* ---- Variable assignment: [export] NAME := VALUE ---- */
        {
            const char *vp  = t;
            int  exported   = 0;
            if (jr_starts_with(vp, "export ")) { exported = 1; vp += 7; vp = jr_ltrim(vp); }

            const char *assign_pos = strstr(vp, ":=");
            if (assign_pos) {
                /* Verify nothing shell-special precedes := (would be a recipe) */
                int ok = 1;
                for (const char *q = vp; q < assign_pos; q++) {
                    if (*q == ':' || *q == '(' || *q == ')') { ok = 0; break; }
                }
                if (ok && jf->n_vars < JR_MAX_VARS) {
                    /* Trim whitespace from name */
                    const char *name_end = assign_pos;
                    while (name_end > vp && (name_end[-1] == ' ' || name_end[-1] == '\t'))
                        name_end--;
                    size_t name_len = (size_t)(name_end - vp);
                    const char *val_start = assign_pos + 2;
                    const char *end;
                    char *raw = parse_rhs_expr_text(val_start, &end);
                    char *val = eval_rhs(raw, jf, path, lineno);
                    free(raw);
                    if (!val) {
                        free(pending_doc);
                        free(line);
                        return 2;
                    }
                    JVar *var = &jf->vars[jf->n_vars++];
                    var->name     = jr_strndup(vp, name_len);
                    var->value    = val;
                    var->exported = exported;
                    if (exported) setenv(var->name, var->value, 0);
                    free(pending_doc);
                    pending_doc = NULL;
                    free(line);
                    continue;
                }
            }
        }

        /* ---- Setting: set NAME := VALUE ---- */
        if (jr_starts_with(t, "set ")) {
            const char *sp = jr_ltrim(t + 4);
            const char *assign_pos = strstr(sp, ":=");
            if (assign_pos) {
                const char *name_end = assign_pos;
                while (name_end > sp && (name_end[-1] == ' ' || name_end[-1] == '\t'))
                    name_end--;
                char *sname = jr_strndup(sp, (size_t)(name_end - sp));
                const char *val_start = assign_pos + 2;
                const char *end;
                char *val = parse_value(val_start, &end);

                if (strcmp(sname, "shell") == 0) {
                    for (int i = 0; i < jf->settings.n_shell; i++)
                        free(jf->settings.shell[i]);
                    jf->settings.n_shell = 0;
                    int ns = 0;
                    char **arr = parse_shell_array(val, &ns);
                    if (arr && ns <= JR_MAX_SHELL) {
                        for (int i = 0; i < ns; i++) jf->settings.shell[i] = arr[i];
                        jf->settings.n_shell = ns;
                        free(arr);
                    } else if (arr) {
                        for (int i = 0; i < ns; i++) free(arr[i]);
                        free(arr);
                    }
                } else if (strcmp(sname, "dotenv-load") == 0) {
                    jf->settings.dotenv_load = (strcmp(val, "true") == 0);
                } else if (strcmp(sname, "positional-arguments") == 0) {
                    jf->settings.positional_arguments = (strcmp(val, "true") == 0);
                }
                /* windows-shell: accepted, silently ignored on POSIX */

                free(sname);
                free(val);
                free(pending_doc);
                pending_doc = NULL;
                free(line);
                continue;
            }
        }

        /* ---- Recipe header: NAME params: deps ---- */
        /* Must start at column 0 */
        if (line[0] != ' ' && line[0] != '\t' && jf->n_recipes < JR_MAX_RECIPES) {
            JRecipe *r = &jf->recipes[jf->n_recipes];
            memset(r, 0, sizeof(*r));
            if (parse_recipe_header(t, r)) {
                r->attrs = pending_attrs;
                /* Resolve [arg(...)] onto the parameters it names.  The
                 * attribute is written above the header, so this is the first
                 * point at which the parameter list exists. */
                for (int a = 0; a < pending_attrs.n_argspecs; a++) {
                    JArgSpec *spec = &pending_attrs.argspecs[a];
                    int matched = 0;
                    for (int q = 0; q < r->n_params; q++) {
                        if (r->params[q].name &&
                            strcmp(r->params[q].name, spec->param) == 0) {
                            r->params[q].opt_long       = spec->lng;
                            r->params[q].opt_short      = spec->shrt;
                            r->params[q].opt_flag_value = spec->flag_value;
                            spec->lng = spec->shrt = spec->flag_value = NULL;
                            matched = 1;
                            break;
                        }
                    }
                    if (!matched) {
                        char *issue = jr_issuef(
                            "%s:%d: [arg('%s')] names a parameter that recipe "
                            "'%s' does not declare", path, lineno,
                            spec->param, r->name);
                        if (jf->n_issues < JR_MAX_ISSUES)
                            jf->issues[jf->n_issues++] = issue;
                        else free(issue);
                    }
                    free(spec->param);
                    free(spec->lng);
                    free(spec->shrt);
                    free(spec->flag_value);
                }
                r->attrs.n_argspecs = 0;
                /* [doc("...")] wins over an accumulated `#` doc comment. */
                if (pending_attrs.doc_attr) {
                    free(pending_doc);
                    r->doc = pending_attrs.doc_attr;
                    r->attrs.doc_attr = NULL;
                } else {
                    r->doc = pending_doc;
                }
                /* `just` omits both [private] recipes and '_'-prefixed ones
                 * from --list; they stay runnable by name. */
                r->hidden = pending_private || (r->name && r->name[0] == '_');
                pending_doc     = NULL;
                pending_private = 0;
                memset(&pending_attrs, 0, sizeof(pending_attrs));
                jf->n_recipes++;
                cur_recipe = r;
                free(line);
                continue;
            }
        }

        /* Unrecognised non-comment line -- reset doc accumulation */
        free(pending_doc);
        pending_doc     = NULL;
        pending_private = 0;
        jattrs_free(&pending_attrs);
        free(line);
    }

    free(pending_doc);
    jattrs_free(&pending_attrs);
    return 0;
}

/* ================================================================== */
/* File utilities                                                      */
/* ================================================================== */

static char *jr_read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Walk upward from cwd to find a Justfile. */
static char *find_justfile(void) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return NULL;

    static const char *NAMES[] = { "Justfile", "justfile", "JUSTFILE" };
    char path[4096];
    for (;;) {
        for (size_t i = 0; i < sizeof(NAMES)/sizeof(NAMES[0]); i++) {
            snprintf(path, sizeof(path), "%s/%s", cwd, NAMES[i]);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                return jr_strdup(path);
        }
        char *slash = strrchr(cwd, '/');
        if (!slash || slash == cwd) break;
        *slash = '\0';
    }
    return NULL;
}

/* ================================================================== */
/* Variable / interpolation environment                                */
/* ================================================================== */

typedef struct {
    const char **names;
    const char **values;
    int          n;
    int          cap;
    /* Heap-allocated values owned by this env (freed by jenv_free). */
    char       **owned;
    int          n_owned;
    int          owned_cap;
} JEnv;

static void jenv_init(JEnv *e) {
    e->cap       = 16;
    e->names     = (const char **)malloc((size_t)e->cap * sizeof(char *));
    e->values    = (const char **)malloc((size_t)e->cap * sizeof(char *));
    e->n         = 0;
    e->owned_cap = 4;
    e->owned     = (char **)malloc((size_t)e->owned_cap * sizeof(char *));
    e->n_owned   = 0;
}

static void jenv_set(JEnv *e, const char *name, const char *value) {
    for (int i = 0; i < e->n; i++) {
        if (strcmp(e->names[i], name) == 0) { e->values[i] = value; return; }
    }
    if (e->n >= e->cap) {
        e->cap *= 2;
        e->names  = (const char **)realloc(e->names,  (size_t)e->cap * sizeof(char *));
        e->values = (const char **)realloc(e->values, (size_t)e->cap * sizeof(char *));
    }
    e->names[e->n]  = name;
    e->values[e->n] = value;
    e->n++;
}

static const char *jenv_get(const JEnv *e, const char *name) {
    for (int i = 0; i < e->n; i++)
        if (strcmp(e->names[i], name) == 0) return e->values[i];
    return NULL;
}

static void jenv_own(JEnv *e, char *val) {
    if (e->n_owned >= e->owned_cap) {
        e->owned_cap *= 2;
        e->owned = (char **)realloc(e->owned, (size_t)e->owned_cap * sizeof(char *));
    }
    e->owned[e->n_owned++] = val;
}

static void jenv_free(JEnv *e) {
    for (int i = 0; i < e->n_owned; i++) free(e->owned[i]);
    free(e->owned);
    free(e->names);
    free(e->values);
    e->n = 0;
}

/* ================================================================== */
/* JFile cleanup                                                       */
/* ================================================================== */

static void jfile_free(JFile *jf) {
    for (int i = 0; i < jf->n_recipes; i++) {
        JRecipe *r = &jf->recipes[i];
        free(r->name);
        free(r->doc);
        jattrs_free(&r->attrs);
        for (int j = 0; j < r->n_params; j++) {
            free(r->params[j].name);
            free(r->params[j].default_val);
            free(r->params[j].opt_long);
            free(r->params[j].opt_short);
            free(r->params[j].opt_flag_value);
        }
        for (int j = 0; j < r->n_deps; j++) {
            free(r->deps[j].recipe);
            for (int k = 0; k < r->deps[j].n_args; k++) free(r->deps[j].args[k]);
            free(r->deps[j].args);
        }
        for (int j = 0; j < r->n_lines; j++) free(r->lines[j].text);
    }
    for (int i = 0; i < jf->n_vars; i++) {
        free(jf->vars[i].name);
        free(jf->vars[i].value);
    }
    for (int i = 0; i < jf->n_aliases; i++) {
        free(jf->aliases[i].name);
        free(jf->aliases[i].target);
    }
    for (int i = 0; i < jf->settings.n_shell; i++) free(jf->settings.shell[i]);
    for (int i = 0; i < jf->n_issues; i++) free(jf->issues[i]);
    free(jf->justfile_dir);
}

/* ================================================================== */
/* Built-in interpolation functions                                    */
/* ================================================================== */

static char *eval_builtin(const char *name, const char **args, int n_args,
                            const JFile *jf, int *failed) {
    if (failed) *failed = 0;
    if (strcmp(name, "env_var") == 0) {
        if (n_args < 1) return jr_strdup("");
        const char *val = getenv(args[0]);
        if (!val) {
            fprintf(stderr, "tur run: env_var: variable '%s' not set\n", args[0]);
            if (failed) *failed = 1;
            return NULL;
        }
        return jr_strdup(val);
    }
    if (strcmp(name, "env_var_or_default") == 0) {
        if (n_args < 2) return jr_strdup(n_args == 1 ? "" : "");
        const char *val = getenv(args[0]);
        return jr_strdup(val ? val : args[1]);
    }
    if (strcmp(name, "os") == 0) {
#if defined(__APPLE__)
        return jr_strdup("macos");
#elif defined(__linux__)
        return jr_strdup("linux");
#elif defined(_WIN32)
        return jr_strdup("windows");
#else
        return jr_strdup("unknown");
#endif
    }
    if (strcmp(name, "os_family") == 0) {
#if defined(_WIN32)
        return jr_strdup("windows");
#else
        return jr_strdup("unix");
#endif
    }
    if (strcmp(name, "path_exists") == 0) {
        if (n_args < 1) return jr_strdup("false");
        struct stat st;
        return jr_strdup(stat(args[0], &st) == 0 ? "true" : "false");
    }
    if (strcmp(name, "replace") == 0) {
        /* replace(s, from, to) -- every occurrence, like just's. */
        if (n_args < 3) return jr_strdup(n_args > 0 ? args[0] : "");
        const char *s = args[0], *from = args[1], *to = args[2];
        size_t flen = strlen(from);
        if (flen == 0) return jr_strdup(s);
        size_t tlen = strlen(to), n = 0;
        for (const char *q = s; (q = strstr(q, from)) != NULL; q += flen) n++;
        char *out = (char *)malloc(strlen(s) + n * (tlen > flen ? tlen - flen : 0) + 1);
        if (!out) { fprintf(stderr, "tur run: out of memory\n"); exit(2); }
        char *w = out;
        for (const char *q = s; *q; ) {
            const char *hit = strstr(q, from);
            if (!hit) { strcpy(w, q); break; }
            memcpy(w, q, (size_t)(hit - q)); w += hit - q;
            memcpy(w, to, tlen);            w += tlen;
            q = hit + flen;
            if (!*q) { *w = '\0'; break; }
        }
        return out;
    }
    if (strcmp(name, "join") == 0) {
        /* join(a, b, ...) -- path join, "/"-separated; an absolute later
         * component replaces what came before, as in just. */
        if (n_args < 1) return jr_strdup("");
        size_t cap = 1;
        for (int i = 0; i < n_args; i++) cap += strlen(args[i]) + 1;
        char *out = (char *)malloc(cap);
        if (!out) { fprintf(stderr, "tur run: out of memory\n"); exit(2); }
        out[0] = '\0';
        for (int i = 0; i < n_args; i++) {
            if (args[i][0] == '/') { strcpy(out, args[i]); continue; }
            size_t len = strlen(out);
            if (len > 0 && out[len - 1] != '/') { out[len++] = '/'; out[len] = '\0'; }
            strcat(out, args[i]);
        }
        return out;
    }
    if (strcmp(name, "error") == 0) {
        /* Abort the run with the author's own message. Returning NULL is how
         * a builtin signals failure to the interpolation layer. */
        fprintf(stderr, "tur run: error: %s\n", n_args > 0 ? args[0] : "");
        if (failed) *failed = 1;
        return NULL;
    }
    if (strcmp(name, "arch") == 0) {
#if defined(__aarch64__) || defined(__arm64__)
        return jr_strdup("aarch64");
#elif defined(__x86_64__) || defined(_M_X64)
        return jr_strdup("x86_64");
#elif defined(__i386__)
        return jr_strdup("x86");
#elif defined(__arm__)
        return jr_strdup("arm");
#else
        return jr_strdup("unknown");
#endif
    }
    if (strcmp(name, "justfile_directory") == 0)
        return jr_strdup(jf->justfile_dir ? jf->justfile_dir : ".");
    if (strcmp(name, "invocation_directory") == 0) {
        char cwd[4096];
        return jr_strdup(getcwd(cwd, sizeof(cwd)) ? cwd : ".");
    }
    if (strcmp(name, "uppercase") == 0) {
        if (n_args < 1) return jr_strdup("");
        char *res = jr_strdup(args[0]);
        for (char *q = res; *q; q++) *q = (char)toupper((unsigned char)*q);
        return res;
    }
    if (strcmp(name, "lowercase") == 0) {
        if (n_args < 1) return jr_strdup("");
        char *res = jr_strdup(args[0]);
        for (char *q = res; *q; q++) *q = (char)tolower((unsigned char)*q);
        return res;
    }
    if (strcmp(name, "trim") == 0) {
        if (n_args < 1) return jr_strdup("");
        return jr_trim(args[0]);
    }
    if (strcmp(name, "quote") == 0) {
        /* Shell-safe single-quote wrapping */
        if (n_args < 1) return jr_strdup("''");
        const char *s  = args[0];
        size_t       n  = strlen(s);
        char        *res = (char *)malloc(n * 4 + 4);
        char        *rp = res;
        *rp++ = '\'';
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '\'') { *rp++ = '\''; *rp++ = '\\'; *rp++ = '\''; *rp++ = '\''; }
            else *rp++ = s[i];
        }
        *rp++ = '\'';
        *rp   = '\0';
        return res;
    }
    return NULL;
}

/* ================================================================== */
/* Interpolation engine                                                */
/* ================================================================== */

/* Evaluate a {{ ... }} expression: variable lookup or function call. */
static char *eval_expr(const char *expr, const JEnv *env, const JFile *jf) {
    char *trimmed = jr_trim(expr);

    /* Function call: name(...) */
    char *paren = strchr(trimmed, '(');
    if (paren && paren > trimmed) {
        *paren = '\0';
        char *fname = jr_trim(trimmed);
        const char *ap = paren + 1;
        char *args[8];
        int   n_args = 0;
        while (*ap && *ap != ')' && n_args < 8) {
            while (*ap == ' ') ap++;
            if (*ap == ')') break;
            const char *end;
            char *arg = parse_value(ap, &end);
            args[n_args++] = arg;
            ap = end;
            while (*ap == ' ') ap++;
            if (*ap == ',') ap++;
        }
        int   bfailed = 0;
        char *result  = eval_builtin(fname, (const char **)args, n_args, jf,
                                     &bfailed);
        for (int i = 0; i < n_args; i++) free(args[i]);
        if (!result && !bfailed)
            fprintf(stderr, "tur run: unknown built-in function '%s'\n", fname);
        free(fname);
        free(trimmed);
        return result;
    }

    /* Variable lookup */
    const char *val = jenv_get(env, trimmed);
    if (!val) val = getenv(trimmed);
    char *result = val ? jr_strdup(val) : jr_strdup("");
    free(trimmed);
    return result;
}

/* Interpolate all {{ ... }} occurrences in a template string. */
static char *interpolate(const char *tmpl, const JEnv *env, const JFile *jf) {
    size_t cap = strlen(tmpl) * 2 + 64;
    char  *res = (char *)malloc(cap);
    size_t len = 0;

#define RES_PUTC(c) do { \
    if (len + 2 >= cap) { cap *= 2; res = (char *)realloc(res, cap); } \
    res[len++] = (char)(c); } while (0)

#define RES_PUTS(s) do { \
    size_t _n = strlen(s); \
    while (len + _n + 1 >= cap) { cap *= 2; res = (char *)realloc(res, cap); } \
    memcpy(res + len, s, _n); len += _n; } while (0)

    const char *p = tmpl;
    while (*p) {
        if (p[0] == '{' && p[1] == '{') {
            p += 2;
            const char *start = p;
            while (*p && !(p[0] == '}' && p[1] == '}')) p++;
            char *expr = jr_strndup(start, (size_t)(p - start));
            char *val  = eval_expr(expr, env, jf);
            free(expr);
            /* A failed expression aborts the interpolation rather than
             * splicing in an empty string: substituting nothing turns
             * `rm -rf {{ env_var('BUILD') }}/x` into `rm -rf /x`, which is
             * exactly the kind of quiet damage an error is for. */
            if (!val) { free(res); return NULL; }
            RES_PUTS(val);
            free(val);
            if (p[0] == '}' && p[1] == '}') p += 2;
        } else {
            RES_PUTC(*p++);
        }
    }
    res[len] = '\0';
    return res;

#undef RES_PUTC
#undef RES_PUTS
}

/* ================================================================== */
/* Recipe lookup                                                       */
/* ================================================================== */

static JRecipe *find_recipe(JFile *jf, const char *name) {
    /* A Justfile may define the same recipe name several times under
     * different platform attributes ([unix] configure / [windows] configure).
     * Prefer one whose platform matches this host; fall back to the first
     * by-name match so a single-definition recipe still resolves (and a
     * genuinely wrong-platform recipe reaches exec_recipe_idx, which reports
     * it properly instead of "not found"). */
    for (int i = 0; i < jf->n_recipes; i++)
        if (jf->recipes[i].name && strcmp(jf->recipes[i].name, name) == 0 &&
            jr_platform_matches(jf->recipes[i].attrs.platform))
            return &jf->recipes[i];
    for (int i = 0; i < jf->n_recipes; i++)
        if (jf->recipes[i].name && strcmp(jf->recipes[i].name, name) == 0)
            return &jf->recipes[i];
    /* Resolve aliases (single hop is enough for typical Justfiles). */
    for (int i = 0; i < jf->n_aliases; i++) {
        if (jf->aliases[i].name && strcmp(jf->aliases[i].name, name) == 0) {
            const char *tgt = jf->aliases[i].target;
            for (int j = 0; j < jf->n_recipes; j++)
                if (jf->recipes[j].name && strcmp(jf->recipes[j].name, tgt) == 0)
                    return &jf->recipes[j];
            return NULL;
        }
    }
    return NULL;
}

/* ================================================================== */
/* Parameter binding                                                   */
/* ================================================================== */

/* Does this recipe bind any parameter to a CLI option? */
static int recipe_has_options(const JRecipe *r) {
    for (int i = 0; i < r->n_params; i++)
        if (r->params[i].opt_long || r->params[i].opt_short) return 1;
    return 0;
}

/* Match argv[k] against one parameter's option spelling.
 * Returns: 0 no match, 1 matched and value is inline (--opt=V, -o=V, or a
 * valueless flag), 2 matched and the value is the NEXT argv entry. */
static int option_matches(const JParam *p, const char *arg, const char **inline_val) {
    *inline_val = NULL;
    const char *body = NULL;
    if (arg[0] == '-' && arg[1] == '-' && p->opt_long) {
        body = arg + 2;
        size_t n = strlen(p->opt_long);
        if (strncmp(body, p->opt_long, n) != 0) return 0;
        if (body[n] == '\0')      body = NULL;         /* exact: --opt */
        else if (body[n] == '=') { *inline_val = body + n + 1; return 1; }
        else return 0;
    } else if (arg[0] == '-' && arg[1] != '-' && arg[1] != '\0' && p->opt_short) {
        body = arg + 1;
        size_t n = strlen(p->opt_short);
        if (strncmp(body, p->opt_short, n) != 0) return 0;
        if (body[n] == '\0')      body = NULL;         /* exact: -o */
        else if (body[n] == '=') { *inline_val = body + n + 1; return 1; }
        else return 0;
    } else {
        return 0;
    }
    (void)body;
    /* Bare option: a flag supplies its own value, otherwise take the next argv. */
    if (p->opt_flag_value) { *inline_val = p->opt_flag_value; return 1; }
    return 2;
}

/* Bind parameters from option-style arguments (`--name value`, `--name=value`,
 * `-n value`, `-n=value`, and valueless flags).  Any argument that is not a
 * recognized option is handed to the positional binder, so a recipe can mix
 * the two.  Mirrors just's [arg(...)] behavior. */
static int bind_option_params(const JRecipe *r, const char **argv, int argc,
                              JEnv *env, const char *recipe_name,
                              const char ***out_rest, int *out_n_rest) {
    const char **rest = (const char **)malloc((size_t)(argc + 1) * sizeof(char *));
    int n_rest = 0;
    int *bound = (int *)calloc((size_t)r->n_params, sizeof(int));
    if (!rest || !bound) { free(rest); free(bound); return 2; }

    for (int k = 0; k < argc; k++) {
        const char *arg = argv[k];
        int handled = 0;
        if (arg[0] == '-' && arg[1] != '\0') {
            for (int i = 0; i < r->n_params; i++) {
                const JParam *p = &r->params[i];
                if (!p->opt_long && !p->opt_short) continue;
                const char *inline_val = NULL;
                int m = option_matches(p, arg, &inline_val);
                if (m == 0) continue;
                if (m == 2) {
                    if (k + 1 >= argc) {
                        fprintf(stderr,
                                "tur run: recipe '%s': option '%s' needs a value\n",
                                recipe_name, arg);
                        free(rest); free(bound);
                        return 2;
                    }
                    inline_val = argv[++k];
                }
                jenv_set(env, p->name, inline_val);
                bound[i] = 1;
                handled  = 1;
                break;
            }
            if (!handled) {
                fprintf(stderr, "tur run: recipe '%s' has no option '%s'\n",
                        recipe_name, arg);
                free(rest); free(bound);
                return 2;
            }
        }
        if (!handled) rest[n_rest++] = arg;
    }

    /* Option-bound parameters that were not supplied fall back to their
     * default, or are reported as required. */
    for (int i = 0; i < r->n_params; i++) {
        const JParam *p = &r->params[i];
        if (!p->opt_long && !p->opt_short) continue;
        if (bound[i]) continue;
        /* An absent flag takes the parameter's default when it has one, and
         * only falls back to empty when it does not -- matching just. */
        if (p->default_val)    { jenv_set(env, p->name, p->default_val); continue; }
        if (p->opt_flag_value) { jenv_set(env, p->name, ""); continue; }
        fprintf(stderr, "tur run: recipe '%s' requires option '%s%s'\n",
                recipe_name,
                p->opt_long ? "--" : "-",
                p->opt_long ? p->opt_long : p->opt_short);
        free(rest); free(bound);
        return 2;
    }

    free(bound);
    *out_rest   = rest;
    *out_n_rest = n_rest;
    return 0;
}

static int bind_params(const JRecipe *r, const char **positional, int n_pos,
                        JEnv *env, const char *recipe_name) {
    /* Option-bound parameters are filled first and removed from the argument
     * list; whatever is left binds positionally as before. */
    const char **owned_rest = NULL;
    if (recipe_has_options(r)) {
        const char **rest = NULL;
        int          n_rest = 0;
        int rc = bind_option_params(r, positional, n_pos, env, recipe_name,
                                    &rest, &n_rest);
        if (rc) return rc;
        owned_rest = rest;
        positional = rest;
        n_pos      = n_rest;
    }

#define BIND_RETURN(rc) do { free(owned_rest); return (rc); } while (0)

    /* Tracked separately from the parameter index: an option-bound parameter
     * consumes no positional slot. */
    int pos_idx = 0;

    for (int i = 0; i < r->n_params; i++) {
        const JParam *p = &r->params[i];
        if (p->opt_long || p->opt_short) continue;  /* already bound */
        if (p->variadic) {
            int remaining = n_pos - pos_idx;
            if (remaining < 0) remaining = 0;
            if (p->variadic_req && remaining == 0) {
                fprintf(stderr,
                    "tur run: recipe '%s' requires at least one argument "
                    "for parameter +%s\n",
                    recipe_name, p->name);
                BIND_RETURN(2);
            }
            size_t total = 0;
            for (int j = pos_idx; j < n_pos; j++) total += strlen(positional[j]) + 1;
            char *joined = (char *)malloc(total + 1);
            char *jp = joined;
            for (int j = pos_idx; j < n_pos; j++) {
                size_t len = strlen(positional[j]);
                memcpy(jp, positional[j], len);
                jp += len;
                if (j + 1 < n_pos) *jp++ = ' ';
            }
            *jp = '\0';
            /* Transfer ownership of 'joined' to env; freed by jenv_free. */
            jenv_own(env, joined);
            jenv_set(env, p->name, joined);
            break;
        } else {
            const char *val = (pos_idx < n_pos) ? positional[pos_idx] : p->default_val;
            if (pos_idx < n_pos) pos_idx++;
            if (!val) {
                fprintf(stderr,
                    "tur run: recipe '%s' requires argument '%s'\n"
                    "  signature: %s",
                    recipe_name, p->name, recipe_name);
                for (int j = 0; j < r->n_params; j++) {
                    if (r->params[j].default_val)
                        fprintf(stderr, " %s='%s'",
                                r->params[j].name, r->params[j].default_val);
                    else
                        fprintf(stderr, " %s", r->params[j].name);
                }
                fprintf(stderr, ":\n");
                BIND_RETURN(2);
            }
            jenv_set(env, p->name, val);
        }
    }
    BIND_RETURN(0);
#undef BIND_RETURN
}

/* ================================================================== */
/* Recipe execution                                                    */
/* ================================================================== */

typedef struct {
    int *executed; /* per-recipe: 1 = already ran this invocation */
} RunCtx;

static int exec_recipe_idx(JFile *jf, int idx, const char **args, int n_args,
                             int dry_run, int verbose, RunCtx *rctx);

static int exec_recipe(JFile *jf, const char *name, const char **args, int n_args,
                        int dry_run, int verbose, RunCtx *rctx) {
    JRecipe *r = find_recipe(jf, name);
    if (!r) {
        fprintf(stderr, "tur run: recipe '%s' not found\n", name);
        if (jf->n_recipes > 0) {
            fprintf(stderr, "  available:");
            for (int i = 0; i < jf->n_recipes; i++)
                fprintf(stderr, " %s", jf->recipes[i].name);
            for (int i = 0; i < jf->n_aliases; i++)
                fprintf(stderr, " %s", jf->aliases[i].name);
            fprintf(stderr, "\n");
        }
        return 2;
    }
    return exec_recipe_idx(jf, (int)(r - jf->recipes), args, n_args,
                            dry_run, verbose, rctx);
}

static int exec_recipe_idx(JFile *jf, int idx, const char **args, int n_args,
                             int dry_run, int verbose, RunCtx *rctx) {
    /* Skip already-executed recipes (each runs at most once per invocation) */
    if (rctx->executed[idx]) return 0;

    JRecipe *r = &jf->recipes[idx];

    /* [unix] / [windows] / ... -- refuse rather than run something written for
     * a different OS.  find_recipe already preferred a matching same-name
     * recipe, so reaching here means no definition applies to this host. */
    if (!jr_platform_matches(r->attrs.platform)) {
        fprintf(stderr,
                "tur run: recipe '%s' is not available on this platform (%s)\n",
                r->name, JR_HOST_OS_NAME);
        return 2;
    }

    /* [confirm] / [confirm("prompt")] -- gate a destructive recipe behind an
     * interactive yes.  A non-tty stdin declines rather than blocking, so a
     * confirm-guarded recipe never silently runs unattended in CI. */
    if (r->attrs.confirm && !dry_run) {
        if (!isatty(STDIN_FILENO)) {
            fprintf(stderr,
                    "tur run: recipe '%s' requires confirmation and stdin is "
                    "not a terminal; declining\n", r->name);
            return 2;
        }
        /* A custom prompt is printed verbatim (it is already a question);
         * only the default form names the recipe. */
        if (r->attrs.confirm_msg)
            fprintf(stderr, "%s [y/N] ", r->attrs.confirm_msg);
        else
            fprintf(stderr, "Run recipe '%s'? [y/N] ", r->name);
        fflush(stderr);
        char resp[16];
        if (!fgets(resp, sizeof(resp), stdin) ||
            (resp[0] != 'y' && resp[0] != 'Y')) {
            fprintf(stderr, "tur run: declined\n");
            return 2;
        }
    }

    /* Build variable environment */
    JEnv env;
    jenv_init(&env);
    for (int i = 0; i < jf->n_vars; i++)
        jenv_set(&env, jf->vars[i].name, jf->vars[i].value);

    /* Bind parameters */
    int rc = bind_params(r, args, n_args, &env, r->name);
    if (rc) { jenv_free(&env); return rc; }

    /* Execute dependencies first */
    for (int i = 0; i < r->n_deps; i++) {
        const JDep *dep = &r->deps[i];
        JRecipe *drec = find_recipe(jf, dep->recipe);
        if (!drec) {
            fprintf(stderr, "tur run: recipe '%s' depends on '%s': not found\n",
                    r->name, dep->recipe);
            jenv_free(&env);
            return 2;
        }
        int dep_idx = (int)(drec - jf->recipes);

        const char **dep_args = NULL;
        if (dep->n_args > 0) {
            dep_args = (const char **)malloc((size_t)dep->n_args * sizeof(char *));
            for (int j = 0; j < dep->n_args; j++) {
                dep_args[j] = interpolate(dep->args[j], &env, jf);
                if (!dep_args[j]) {
                    fprintf(stderr,
                            "tur run: recipe '%s': failed to evaluate argument "
                            "to dependency '%s'\n", r->name, dep->recipe);
                    for (int k = 0; k < j; k++) free((char *)dep_args[k]);
                    free(dep_args);
                    jenv_free(&env);
                    return 2;
                }
            }
        }
        rc = exec_recipe_idx(jf, dep_idx, dep_args, dep->n_args,
                              dry_run, verbose, rctx);
        if (dep_args) {
            for (int j = 0; j < dep->n_args; j++) free((char *)dep_args[j]);
            free(dep_args);
        }
        if (rc) { jenv_free(&env); return rc; }
    }

    /* Mark executed before running the body (prevents re-runs in circular
     * dep graphs that the parser didn't catch). */
    rctx->executed[idx] = 1;

    if (verbose) fprintf(stderr, "[tur run] running recipe: %s\n", r->name);

    /* Shebang recipe: if the first body line starts with `#!`, the whole body
     * is a single script run by the interpreter named on the shebang line
     * (e.g. `#!/usr/bin/env bash`), not a sequence of independent `sh -c`
     * lines.  This matches `just` semantics and is required for recipes that
     * rely on bash features (`set -o pipefail`, process substitution) or that
     * carry state across lines.  We materialize the interpolated body into a
     * temp file, mark it executable, and exec it so the kernel honors the
     * shebang. */
    if (r->n_lines > 0 && r->lines[0].text &&
        r->lines[0].text[0] == '#' && r->lines[0].text[1] == '!') {
        char tmpl[] = "/tmp/tur-run-XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) {
            fprintf(stderr, "tur run: failed to create temp script for recipe '%s'\n",
                    r->name);
            jenv_free(&env);
            return 2;
        }
        int write_err = 0;
        for (int i = 0; i < r->n_lines; i++) {
            char *cmd = interpolate(r->lines[i].text, &env, jf);
            if (!cmd) { write_err = 1; break; }
            size_t len = strlen(cmd);
            if ((len && write(fd, cmd, len) != (ssize_t)len) ||
                write(fd, "\n", 1) != 1) {
                write_err = 1;
            }
            free(cmd);
            if (write_err) break;
        }
        close(fd);
        if (write_err) {
            unlink(tmpl);
            jenv_free(&env);
            return 2;
        }
        if (chmod(tmpl, 0700) != 0) {
            unlink(tmpl);
            jenv_free(&env);
            return 2;
        }
        int exit_code = 0;
        if (!dry_run) {
            int sys_rc = system(tmpl);
            exit_code = WIFEXITED(sys_rc) ? WEXITSTATUS(sys_rc) : 1;
        }
        unlink(tmpl);
        jenv_free(&env);
        return exit_code;
    }

    /* Execute body lines */
    for (int i = 0; i < r->n_lines; i++) {
        const JLine *jl = &r->lines[i];

        char *cmd = interpolate(jl->text, &env, jf);
        if (!cmd) { jenv_free(&env); return 2; }

        if (!jl->silent) {
            printf("%s\n", cmd);
            fflush(stdout);
        }

        if (!dry_run) {
            int sys_rc = system(cmd);
            int exit_code = WIFEXITED(sys_rc) ? WEXITSTATUS(sys_rc) : 1;
            if (exit_code != 0 && !jl->cont) {
                free(cmd);
                jenv_free(&env);
                return exit_code;
            }
        }
        free(cmd);
    }

    jenv_free(&env);
    return 0;
}

/* ================================================================== */
/* Listing                                                             */
/* ================================================================== */

/* Write `s` to stdout as the body of a JSON string (no surrounding quotes),
 * escaping everything RFC 8259 requires.  Every string field in the --list
 * --json output goes through this -- a recipe name or a parameter default is
 * just as capable of containing a quote as a doc comment is. */
static void jr_json_puts(const char *s) {
    if (!s) return;
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) {
        switch (*q) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n",  stdout); break;
            case '\r': fputs("\\r",  stdout); break;
            case '\t': fputs("\\t",  stdout); break;
            case '\b': fputs("\\b",  stdout); break;
            case '\f': fputs("\\f",  stdout); break;
            default:
                if (*q < 0x20) printf("\\u%04x", (unsigned)*q);
                else           putchar((char)*q);
                break;
        }
    }
}

/* Resolve an alias name to its target recipe, or NULL. */
static const JRecipe *alias_target(const JFile *jf, const char *target) {
    for (int i = 0; i < jf->n_recipes; i++)
        if (jf->recipes[i].name && strcmp(jf->recipes[i].name, target) == 0)
            return &jf->recipes[i];
    return NULL;
}

/* An alias inherits its target's visibility, and hides on its own name too. */
static int alias_hidden(const JFile *jf, const JAlias *a) {
    if (a->name && a->name[0] == '_') return 1;
    const JRecipe *t = alias_target(jf, a->target);
    return t ? t->hidden : 0;
}

/* One plain-text listing line: name, params, trailing doc comment. */
static void list_one_recipe(const JRecipe *r) {
    printf("  %s", r->name);
    for (int j = 0; j < r->n_params; j++) {
        const JParam *p = &r->params[j];
        if (p->variadic_req)        printf(" +%s", p->name);
        else if (p->variadic)       printf(" *%s", p->name);
        else if (p->default_val)    printf(" %s='%s'", p->name, p->default_val);
        else                        printf(" %s", p->name);
    }
    if (r->doc) {
        const char *nl = strchr(r->doc, '\n');
        if (nl) printf("  # %.*s", (int)(nl - r->doc), r->doc);
        else    printf("  # %s", r->doc);
    }
    printf("\n");
}

static int list_recipes(const JFile *jf, int json_mode, int show_all) {
    if (json_mode) {
        /* Count first so the trailing-comma logic stays correct across the
         * two sections (recipes, then aliases). */
        int total = 0;
        for (int i = 0; i < jf->n_recipes; i++)
            if (show_all || !jf->recipes[i].hidden) total++;
        for (int i = 0; i < jf->n_aliases; i++)
            if (show_all || !alias_hidden(jf, &jf->aliases[i])) total++;

        int emitted = 0;
        printf("[\n");
        for (int i = 0; i < jf->n_recipes; i++) {
            const JRecipe *r = &jf->recipes[i];
            if (!show_all && r->hidden) continue;
            printf("  {\"name\":\"");
            jr_json_puts(r->name);
            printf("\"");
            if (r->hidden) printf(",\"hidden\":true");
            if (r->attrs.group) {
                printf(",\"group\":\"");
                jr_json_puts(r->attrs.group);
                printf("\"");
            }
            if (r->doc) {
                printf(",\"doc\":\"");
                jr_json_puts(r->doc);
                printf("\"");
            }
            if (r->n_params > 0) {
                printf(",\"params\":[");
                for (int j = 0; j < r->n_params; j++) {
                    if (j) printf(",");
                    printf("{\"name\":\"");
                    jr_json_puts(r->params[j].name);
                    printf("\"");
                    if (r->params[j].default_val) {
                        printf(",\"default\":\"");
                        jr_json_puts(r->params[j].default_val);
                        printf("\"");
                    }
                    if (r->params[j].variadic) printf(",\"variadic\":true");
                    printf("}");
                }
                printf("]");
            }
            printf("}%s\n", (++emitted < total) ? "," : "");
        }
        /* Aliases are runnable names (find_recipe resolves them), so they
         * belong in the machine-readable listing that drives completion. */
        for (int i = 0; i < jf->n_aliases; i++) {
            const JAlias  *a = &jf->aliases[i];
            if (!show_all && alias_hidden(jf, a)) continue;
            const JRecipe *t = alias_target(jf, a->target);
            printf("  {\"name\":\"");
            jr_json_puts(a->name);
            printf("\",\"alias\":\"");
            jr_json_puts(a->target);
            printf("\"");
            if (t && t->doc) {
                printf(",\"doc\":\"");
                jr_json_puts(t->doc);
                printf("\"");
            }
            printf("}%s\n", (++emitted < total) ? "," : "");
        }
        printf("]\n");
        return 0;
    }

    /* Plain text.  Ungrouped recipes first, then one section per [group(...)]
     * in order of first appearance -- the flat list is what makes a 60-recipe
     * Justfile unreadable. */
    for (int i = 0; i < jf->n_recipes; i++) {
        const JRecipe *r = &jf->recipes[i];
        if (!show_all && r->hidden) continue;
        if (r->attrs.group) continue;
        list_one_recipe(r);
    }
    for (int i = 0; i < jf->n_recipes; i++) {
        const char *g = jf->recipes[i].attrs.group;
        if (!g) continue;
        if (!show_all && jf->recipes[i].hidden) continue;
        /* Only emit the section at the group's first visible member. */
        int first = 1;
        for (int j = 0; j < i; j++) {
            const JRecipe *pr = &jf->recipes[j];
            if (!show_all && pr->hidden) continue;
            if (pr->attrs.group && strcmp(pr->attrs.group, g) == 0) { first = 0; break; }
        }
        if (!first) continue;
        printf("\n  [%s]\n", g);
        for (int j = i; j < jf->n_recipes; j++) {
            const JRecipe *m = &jf->recipes[j];
            if (!show_all && m->hidden) continue;
            if (!m->attrs.group || strcmp(m->attrs.group, g) != 0) continue;
            list_one_recipe(m);
        }
    }
    for (int i = 0; i < jf->n_aliases; i++) {
        const JAlias *a = &jf->aliases[i];
        if (!show_all && alias_hidden(jf, a)) continue;
        printf("  %s  # alias for `%s`\n", a->name, a->target);
    }
    return 0;
}

/* ================================================================== */
/* Justfile template (--init / tur new)                                */
/* ================================================================== */

static const char JUSTFILE_TEMPLATE[] =
"# Justfile for {{ spice_name }}\n"
"#\n"
"# Run `tur run --list` for the full set of recipes.\n"
"# Add your own recipes below; the ones above are the contract that the\n"
"# spice template, CI, and `tur publish` rely on.\n"
"\n"
"# Default to listing recipes; override for your most common task if you like.\n"
"default:\n"
"    @tur run --list\n"
"\n"
"# Build the spice (debug profile).\n"
"build:\n"
"    tur build .\n"
"\n"
"# Build with optimizations.\n"
"release:\n"
"    tur build --release .\n"
"\n"
"# Run the spice's test suite; depends on a fresh debug build.\n"
"test: build\n"
"    tur test tests/\n"
"\n"
"# Re-build on source changes; useful while iterating.\n"
"watch:\n"
"    tur build --watch\n"
"\n"
"# Remove build artifacts and the local cache.\n"
"clean:\n"
"    rm -rf build/ .tur-cache/\n"
"\n"
"# Generate the per-spice API docs from `;;;` docstrings.\n"
"# NOTE: a native, spice-aware `tur docs` generator is not wired up yet, so\n"
"# this is a placeholder that succeeds. It is intentionally NOT in the `ci`\n"
"# chain below until `tur docs` lands.\n"
"docs:\n"
"    @echo \"docs: per-spice doc generation is not available yet "
        "(tracked: native 'tur docs').\"\n"
"\n"
"# Format sources in place.\n"
"fmt:\n"
"    tur fmt src/ tests/\n"
"\n"
"# Type-check / lint without producing an artifact, and verify style.\n"
"check:\n"
"    tur check src/\n"
"    tur fmt --check src/ tests/\n"
"\n"
"# Tag a release: `tur run tag 0.2.0` produces `{{ spice_name }}-v0.2.0`.\n"
"tag VERSION:\n"
"    git tag -a \"{{ spice_name }}-v{{ VERSION }}\" "
        "-m \"{{ spice_name }} v{{ VERSION }}\"\n"
"    @echo \"Tagged {{ spice_name }}-v{{ VERSION }}. "
        "Push with: git push --tags\"\n"
"\n"
"# Install this spice into the local registry for downstream testing.\n"
"install:\n"
"    tur install .\n"
"\n"
"# CI entry point: clean + check + test. Used by the default GitHub Actions\n"
"# workflow that `tur new` also scaffolds. `docs` is intentionally omitted\n"
"# until a native `tur docs` generator lands (run it manually with\n"
"# `tur run docs`).\n"
"ci: clean check test\n";

int justrun_write_template(const char *dir, const char *spice_name, int force) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/Justfile", dir);

    struct stat st;
    if (stat(path, &st) == 0 && !force) {
        fprintf(stderr,
            "tur run: Justfile already exists at '%s'\n"
            "  Use --force to overwrite, or merge missing recipes manually.\n",
            path);
        return 1;
    }

    /* Resolve spice name */
    char resolved[256];
    if (spice_name) {
        strncpy(resolved, spice_name, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    } else {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");
        const char *base = strrchr(cwd, '/');
        strncpy(resolved, base ? base + 1 : cwd, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }

    /* Substitute {{ spice_name }} in the template */
    const char *PLACEHOLDER = "{{ spice_name }}";
    size_t ph_len = strlen(PLACEHOLDER);
    size_t nm_len = strlen(resolved);
    const char *tmpl = JUSTFILE_TEMPLATE;
    size_t tmpl_len  = strlen(tmpl);

    /* Count occurrences to size the output buffer */
    size_t occ = 0;
    for (const char *q = tmpl; (q = strstr(q, PLACEHOLDER)); q += ph_len) occ++;
    size_t out_cap = tmpl_len + occ * nm_len + 1;
    char  *output  = (char *)malloc(out_cap);
    char  *op = output;
    const char *p = tmpl;
    while (*p) {
        if (jr_starts_with(p, PLACEHOLDER)) {
            memcpy(op, resolved, nm_len);
            op += nm_len;
            p  += ph_len;
        } else {
            *op++ = *p++;
        }
    }
    *op = '\0';

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "tur run: cannot write '%s': %s\n", path, strerror(errno));
        free(output);
        return 1;
    }
    fputs(output, f);
    fclose(f);
    free(output);

    printf("Wrote %s\n", path);
    printf("Run 'tur run --list' to see available recipes.\n");
    return 0;
}

/* ================================================================== */
/* Usage                                                               */
/* ================================================================== */

int usage_justrun(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur run                        list recipes (or run 'default')\n"
        "  tur run --list                 list all recipes with doc + params\n"
        "  tur run --list --json          machine-readable recipe list\n"
        "  tur run --list --all           include [private] / _-prefixed recipes\n"
        "  tur run <recipe> [args...]     run a Justfile recipe\n"
        "  tur run --dry-run <recipe>     print resolved commands, don't exec\n"
        "  tur run --verbose <recipe>     echo recipe metadata to stderr\n"
        "  tur run --justfile <path> <r>  use an explicit Justfile\n"
        "  tur run --chdir <dir> <r>      run recipe from <dir>\n"
        "  tur run --init                 write a starter Justfile into cwd\n"
        "  tur run --init --force         overwrite an existing Justfile\n"
        "  tur run <file.tur> [-- <args>] compile and run a .tur file\n"
        "\n"
        "flags (task-runner mode):\n"
        "  --list, -l          list recipes (aliases included; hidden ones are not)\n"
        "  --all, -a           with --list: also show [private] / _-prefixed recipes\n"
        "  --json              JSON output (with --list)\n"
        "  --dry-run           print resolved commands; do not execute\n"
        "  --verbose           echo recipe metadata to stderr\n"
        "  --justfile PATH     explicit Justfile path\n"
        "  --chdir DIR         change to DIR before running the recipe\n"
        "  --set VAR VALUE     override a Justfile variable (also VAR=VALUE)\n"
        "  --init              write a starter Justfile into cwd\n"
        "  --force             overwrite existing Justfile (with --init)\n"
        "\n"
        "exit codes:\n"
        "  0    recipe succeeded\n"
        "  1    recipe exited non-zero (exit code propagated)\n"
        "  2    CLI / parse / unsupported-feature error\n"
        "  127  Justfile not found\n"
        "\n"
        "supported Justfile subset: recipes, deps, params, variables,\n"
        "{{ }} interpolation, @ and - prefixes, set shell / dotenv-load /\n"
        "positional-arguments, built-in functions (env_var, os, arch, ...).\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

/* ================================================================== */
/* CLI entry point                                                     */
/* ================================================================== */

int justrun_finds_justfile(void) {
    char *p = find_justfile();
    if (!p) return 0;
    free(p);
    return 1;
}

int cmd_justrun(int argc, char **argv) {
    int         list_mode        = 0;
    int         json_output      = 0;
    int         dry_run          = 0;
    int         verbose          = 0;
    int         init_mode        = 0;
    int         force            = 0;
    int         show_all         = 0;
    const char *explicit_just    = NULL;
    const char *chdir_to         = NULL;
    const char *recipe_name      = NULL;
    const char **recipe_args     = NULL;
    int          n_recipe_args   = 0;
    int          end_of_opts     = 0;
    /* --set overrides, applied after the parse so they win over the file. */
    const char  *set_names[JR_MAX_VARS];
    const char  *set_values[JR_MAX_VARS];
    int          n_sets           = 0;

    for (int i = 2; i < argc; i++) {
        if (!end_of_opts && strcmp(argv[i], "--") == 0) {
            end_of_opts = 1;
            /* Everything after -- are passthrough args for the recipe */
            if (i + 1 < argc) {
                recipe_args   = (const char **)(argv + i + 1);
                n_recipe_args = argc - i - 1;
            }
            break;
        }
        if (!end_of_opts && argv[i][0] == '-') {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
                return usage_justrun();
            if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
                list_mode = 1; continue;
            }
            if (strcmp(argv[i], "--all") == 0 || strcmp(argv[i], "-a") == 0) {
                show_all = 1; continue;
            }
            if (strcmp(argv[i], "--json") == 0) { json_output = 1; continue; }
            if (strcmp(argv[i], "--dry-run") == 0) { dry_run = 1; continue; }
            if (strcmp(argv[i], "--verbose") == 0) { verbose = 1; continue; }
            if (strcmp(argv[i], "--init") == 0) { init_mode = 1; continue; }
            if (strcmp(argv[i], "--force") == 0) { force = 1; continue; }
            /* --set VAR VALUE, and the --set VAR=VALUE spelling. */
            if (strcmp(argv[i], "--set") == 0) {
                if (n_sets >= JR_MAX_VARS) {
                    fprintf(stderr, "tur run: too many --set overrides\n");
                    return 2;
                }
                if (i + 2 < argc && strchr(argv[i + 1], '=') == NULL) {
                    set_names[n_sets]    = argv[i + 1];
                    set_values[n_sets++] = argv[i + 2];
                    i += 2;
                    continue;
                }
                if (i + 1 < argc) {
                    char *eq = strchr(argv[i + 1], '=');
                    if (eq) {
                        *eq = '\0';  /* argv is writable and not reused */
                        set_names[n_sets]    = argv[i + 1];
                        set_values[n_sets++] = eq + 1;
                        i += 1;
                        continue;
                    }
                }
                fprintf(stderr, "tur run: --set needs VAR VALUE (or VAR=VALUE)\n");
                return 2;
            }
            if (strcmp(argv[i], "--justfile") == 0 && i + 1 < argc) {
                explicit_just = argv[++i]; continue;
            }
            if (strcmp(argv[i], "--chdir") == 0 && i + 1 < argc) {
                chdir_to = argv[++i]; continue;
            }
            fprintf(stderr, "tur run: unknown option '%s'\n", argv[i]);
            return usage_justrun();
        }
        /* First positional = recipe name; rest = recipe args */
        if (!recipe_name) {
            recipe_name   = argv[i];
            recipe_args   = (const char **)(argv + i + 1);
            n_recipe_args = argc - i - 1;
        }
        break;
    }

    /* --init mode */
    if (init_mode)
        return justrun_write_template(".", NULL, force);

    /* --chdir */
    if (chdir_to && chdir(chdir_to) != 0) {
        fprintf(stderr, "tur run: cannot chdir to '%s': %s\n",
                chdir_to, strerror(errno));
        return 2;
    }

    /* Locate and read the Justfile */
    char *justfile_path = explicit_just ? jr_strdup(explicit_just) : find_justfile();
    if (!justfile_path) {
        fprintf(stderr,
            "tur run: no Justfile found (searched upward from cwd)\n"
            "  Run 'tur run --init' to create a starter Justfile, or\n"
            "  pass --justfile <path> to specify one.\n");
        return 127;
    }

    char *text = jr_read_file(justfile_path);
    if (!text) {
        fprintf(stderr, "tur run: cannot read '%s': %s\n",
                justfile_path, strerror(errno));
        free(justfile_path);
        return 127;
    }

    /* Derive justfile_dir */
    char justfile_dir[4096];
    strncpy(justfile_dir, justfile_path, sizeof(justfile_dir) - 1);
    justfile_dir[sizeof(justfile_dir) - 1] = '\0';
    {
        char *sl = strrchr(justfile_dir, '/');
        if (sl) *sl = '\0'; else strcpy(justfile_dir, ".");
    }

    /* Parse */
    JFile jf;
    memset(&jf, 0, sizeof(jf));
    jf.settings.n_shell   = 2;
    jf.settings.shell[0]  = jr_strdup("sh");
    jf.settings.shell[1]  = jr_strdup("-c");
    jf.justfile_dir       = jr_strdup(justfile_dir);

    int rc = parse_justfile(text, justfile_path, &jf);
    free(text);
    if (rc) { jfile_free(&jf); free(justfile_path); return rc; }

    /* --set overrides: applied after parsing so they beat the file's own
     * assignment, and added if the name is new (matching `just --set`). */
    for (int s = 0; s < n_sets; s++) {
        int found = 0;
        for (int i = 0; i < jf.n_vars; i++) {
            if (jf.vars[i].name && strcmp(jf.vars[i].name, set_names[s]) == 0) {
                free(jf.vars[i].value);
                jf.vars[i].value = jr_strdup(set_values[s]);
                found = 1;
                break;
            }
        }
        if (!found && jf.n_vars < JR_MAX_VARS) {
            jf.vars[jf.n_vars].name     = jr_strdup(set_names[s]);
            jf.vars[jf.n_vars].value    = jr_strdup(set_values[s]);
            jf.vars[jf.n_vars].exported = 0;
            jf.n_vars++;
        }
    }

    /* .env loading */
    if (jf.settings.dotenv_load) load_dotenv(justfile_dir);

    /* --list or no recipe + no default */
    if (list_mode || (!recipe_name && !find_recipe(&jf, "default"))) {
        /* Unsupported features degrade a listing rather than killing it: the
         * recipes we could parse are still worth reporting (shell completion
         * depends on this), and the notes go to stderr so stdout stays clean
         * for a JSON consumer. */
        for (int i = 0; i < jf.n_issues; i++)
            fprintf(stderr, "tur run: note: %s\n", jf.issues[i]);
        int r = list_recipes(&jf, json_output || use_json_output, show_all);
        jfile_free(&jf);
        free(justfile_path);
        return r;
    }

    /* Executing a recipe out of a Justfile we only partly understand would run
     * the wrong thing, so here the recorded issues are fatal. */
    if (jf.n_issues > 0) {
        for (int i = 0; i < jf.n_issues; i++)
            fprintf(stderr, "tur run: %s\n", jf.issues[i]);
        jfile_free(&jf);
        free(justfile_path);
        return 2;
    }

    /* Run the target recipe */
    const char *target = recipe_name ? recipe_name : "default";

    RunCtx rctx;
    rctx.executed = (int *)calloc((size_t)jf.n_recipes, sizeof(int));
    if (!rctx.executed) { jfile_free(&jf); free(justfile_path); return 2; }

    rc = exec_recipe(&jf, target, recipe_args, n_recipe_args,
                     dry_run, verbose, &rctx);

    free(rctx.executed);
    jfile_free(&jf);
    free(justfile_path);
    return rc;
}
