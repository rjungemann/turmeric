/* experiments.c -- the experimental-feature-flag registry.
 *
 * Successor to the retired `-X<name>` surface.  See experiments.h and
 * docs/upcoming/v1/experimental-flag-mechanism-plan.md.
 *
 * The EXPERIMENTS[] table below is the single source of truth: `tur
 * experiments`, `tur --help`, the docs site, and the release-cut script all
 * read it, and nothing restates the list.  To add a feature, append a row with
 * all seven fields populated and point `opt_global` at a `g_opt_<name>` bool the
 * feature's elaboration reads (keep the trailing `{ 0 }` sentinel last).
 *
 * Shape of a future entry (do not uncomment -- illustrative only):
 *
 *   static const ExperimentDescriptor EXPERIMENTS[] = {
 *     { "fancy-rows",
 *       "extensible row-typed records",
 *       "docs/upcoming/v1/fancy-rows-plan.md",
 *       "0.25.0",                  // introduced
 *       "0.28.0",                  // expires_at (hard contract; release-cut enforced)
 *       XF_LIFECYCLE_PROTOTYPE,
 *       &g_opt_fancy_rows },
 *   };
 */
#include "experiments.h"
#include "globals.h"   /* g_opt_<name> enable bits */

#include <stdio.h>
#include <stdlib.h>   /* getenv, exit, malloc, free */
#include <string.h>

/* The registry.  Empty by design (see file header). */
static const ExperimentDescriptor EXPERIMENTS[] = {
    /* defstruct-as-defadt GRADUATED 2026-06-28 -- a `defstruct` now lowers to a
     * single-variant record `defadt` unconditionally (always-on; the gate lives
     * in defstruct_lowers_to_adt, elab_structs.c).  See
     * docs/upcoming/defstruct-as-defadt-plan.md. */
    /* B4 byvalue-recursive-carrier GRADUATED 2026-06-25 -- the recursive carrier
     * wrappers (Re/Expr, and wider products carrying an (F Self) field) now flow
     * by value through the fat-closure ABI unconditionally; the gate lives in
     * adt_is_byvalue_product_d (types.c).  See
     * docs/upcoming/v2/b4-fat-closure-byvalue-adt-abi-plan.md. */
    { "forall-kinds",
      "explicit kind annotations on forall/exists bound variables, e.g. (f :: * -> *)",
      "docs/upcoming/v1/constrained-hkt-forall-plan.md",
      "0.25.6",                    /* introduced */
      "0.27.0",                    /* expires_at (hard contract; release-cut enforced) */
      XF_LIFECYCLE_PROTOTYPE,
      &g_opt_forall_kinds },
    { "forall-constraints",
      "typeclass constraint vectors on forall types, e.g. (forall [a] [(Show a)] (-> a cstr)), enforced at rank-2 instantiation sites",
      "docs/upcoming/v1/constrained-hkt-forall-plan.md",
      "0.25.6",                    /* introduced */
      "0.27.0",                    /* expires_at (hard contract; release-cut enforced) */
      XF_LIFECYCLE_PROTOTYPE,
      &g_opt_forall_constraints },
    { "hkt-hrt",
      "rank-2 forall over a higher-kinded variable, e.g. (forall [(f :: * -> *)] (-> (f int) int)), validated at instantiation sites",
      "docs/upcoming/v1/constrained-hkt-forall-plan.md",
      "0.25.6",                    /* introduced */
      "0.27.0",                    /* expires_at (hard contract; release-cut enforced) */
      XF_LIFECYCLE_PROTOTYPE,
      &g_opt_hkt_hrt },
    { "forall-dict-pass",
      "runtime dictionary passing for a polymorphic constrained function used as a rank-2 argument (mode B)",
      "docs/upcoming/v1/constrained-hkt-forall-mode-b-plan.md",
      "0.25.6",                    /* introduced */
      "0.27.0",                    /* expires_at (hard contract; release-cut enforced) */
      XF_LIFECYCLE_PROTOTYPE,
      &g_opt_forall_dict_pass },
    { "hrt-curried-result",
      "curried rank-2 poly fn whose forall body result is itself a function, e.g. (forall [a] (-> a (-> a a))), so (l x) yields a callable closure (mode B)",
      "docs/upcoming/v1/constrained-hkt-forall-mode-b-plan.md",
      "0.25.6",                    /* introduced */
      "0.27.0",                    /* expires_at (hard contract; release-cut enforced) */
      XF_LIFECYCLE_PROTOTYPE,
      &g_opt_hrt_curried_result },
    { "vl-wide-functor",
      "van Laarhoven lens focusing through a WIDE by-value functor (a :copy struct / flat-product ADT wider than the one-int64 carrier), boxed across the lens crossings; lifts TUR-E0309 (Path A of van-laarhoven-wide-functor-carrier-plan)",
      "docs/upcoming/v1/van-laarhoven-wide-functor-carrier-plan.md",
      "0.26.1",                    /* introduced */
      "0.29.0",                    /* expires_at (hard contract; release-cut enforced) */
      XF_LIFECYCLE_PROTOTYPE,
      &g_opt_vl_wide_functor },
    { 0 }, /* sentinel so the array is never zero-length (C forbids that);
            * experiment_count() subtracts it off. */
};

/* Number of real entries (excluding the trailing { 0 } sentinel). */
size_t experiment_count(void) {
    return (sizeof(EXPERIMENTS) / sizeof(EXPERIMENTS[0])) - 1;
}

const ExperimentDescriptor *experiment_at(size_t i) {
    if (i >= experiment_count()) return NULL;
    return &EXPERIMENTS[i];
}

const ExperimentDescriptor *experiment_lookup(const char *name) {
    if (!name) return NULL;
    size_t n = experiment_count();
    for (size_t i = 0; i < n; i++) {
        if (strcmp(EXPERIMENTS[i].name, name) == 0) return &EXPERIMENTS[i];
    }
    return NULL;
}

/* Per-index enable source + once-per-compile warning dedup.  Sized to a fixed
 * cap that comfortably exceeds any sane table size (the mechanism is designed
 * so no more than ~2 flags live here at once). */
#define XF_MAX 64
static ExperimentSource g_src[XF_MAX];   /* XF_SRC_NONE = disabled */
static bool             g_warned[XF_MAX]; /* TUR-W006x emitted this compile */

static long experiment_index(const char *name) {
    if (!name) return -1;
    size_t n = experiment_count();
    for (size_t i = 0; i < n; i++) {
        if (strcmp(EXPERIMENTS[i].name, name) == 0) return (long)i;
    }
    return -1;
}

bool experiment_enable(const char *name, ExperimentSource src) {
    long idx = experiment_index(name);
    if (idx < 0 || idx >= XF_MAX) return false;
    const ExperimentDescriptor *d = &EXPERIMENTS[idx];
    if (d->opt_global) *d->opt_global = true;
    /* Higher-numbered source wins.  CLI beats manifest beats user-config
     * beats not-yet-set (XF_SRC_NONE); a lower-precedence enable never
     * downgrades a higher one that already ran. */
    if (src > g_src[idx]) g_src[idx] = src;
    return true;
}

bool experiment_is_enabled(const char *name) {
    long idx = experiment_index(name);
    if (idx < 0 || idx >= XF_MAX) return false;
    return g_src[idx] != XF_SRC_NONE;
}

ExperimentSource experiment_source_at(size_t i) {
    if (i >= experiment_count() || i >= XF_MAX) return XF_SRC_NONE;
    return g_src[i];
}

/* UC-4 (user-config-experiments-plan): the TUR-W006x lifecycle warnings now
 * fire unconditionally.  Enabling an experiment (via --enable=<name>,
 * build.tur, or ~/.config/turmeric/experiments.tur) is itself the
 * acknowledgment; there is no longer a --allow-experimental gate to suppress
 * them. */
void experiment_warn_if_used(const char *name) {
    long idx = experiment_index(name);
    if (idx < 0 || idx >= XF_MAX) return;
    if (g_src[idx] == XF_SRC_NONE) return;   /* not enabled -> nothing to warn */
    if (g_warned[idx]) return;               /* once per compile */
    g_warned[idx] = true;
    const ExperimentDescriptor *d = &EXPERIMENTS[idx];
    if (d->lifecycle == XF_LIFECYCLE_BETA) {
        fprintf(stderr,
                "warning [TUR-W0061]: experimental feature '%s' (beta) -- "
                "graduates in %s; see %s\n",
                d->name, d->expires_at, d->plan_path);
    } else {
        fprintf(stderr,
                "warning [TUR-W0060]: experimental feature '%s' (prototype) -- "
                "breaking changes likely; see %s\n",
                d->name, d->plan_path);
    }
}

void experiment_reset_warnings(void) {
    memset(g_warned, 0, sizeof(g_warned));
}

/* ------------------------------------------------------------------------- *
 * UC-2: user-level experiments file reader.
 *
 * The file's grammar is a tiny subset of turmeric syntax -- a handful of
 * `:key [atom ...]` pairs, with `;` line comments and `#| |#` block comments.
 * A dedicated hand reader (no manifest reader, no `.tur` evaluation) keeps the
 * surface understandable and free of project-manifest assumptions; see the
 * plan's "The reader" section (option B).
 * ------------------------------------------------------------------------- */

/* Resolve the platform path to the user-level experiments file into `buf`.
 * Returns true (and fills `buf`) when a config directory is known -- the file
 * itself may or may not exist -- and false when no home/config dir can be
 * determined. */
static bool uc_config_path(char *buf, size_t buflen) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && appdata[0]) {
        int n = snprintf(buf, buflen, "%s\\turmeric\\experiments.tur", appdata);
        return n > 0 && (size_t)n < buflen;
    }
    return false;
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        int n = snprintf(buf, buflen, "%s/turmeric/experiments.tur", xdg);
        return n > 0 && (size_t)n < buflen;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        int n = snprintf(buf, buflen, "%s/.config/turmeric/experiments.tur", home);
        return n > 0 && (size_t)n < buflen;
    }
    return false;
#endif
}

/* Slurp a whole file into a NUL-terminated malloc'd buffer.  Returns NULL if
 * the file cannot be opened (absent / unreadable) or on allocation failure.
 * The caller frees. */
static char *uc_slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

typedef enum { UC_EOF, UC_LBRACK, UC_RBRACK, UC_ATOM } UcTok;

static bool uc_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

/* Advance `*pp` past whitespace, `;' line comments, and `#| |#' block
 * comments (non-nesting, matching the manifest reader's convention). */
static void uc_skip_ws(const char **pp) {
    const char *p = *pp;
    for (;;) {
        while (uc_is_space(*p)) p++;
        if (*p == ';') {                         /* line comment to EOL */
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '#' && p[1] == '|') {        /* block comment */
            p += 2;
            while (*p && !(p[0] == '|' && p[1] == '#')) p++;
            if (*p) p += 2;
            continue;
        }
        break;
    }
    *pp = p;
}

/* Lex one token.  For UC_ATOM, [*ts, *te) delimits the atom text (a leading
 * ':' is kept, marking a keyword).  Brackets carry no text. */
static UcTok uc_next(const char **pp, const char **ts, const char **te) {
    uc_skip_ws(pp);
    const char *p = *pp;
    if (*p == '\0') return UC_EOF;
    if (*p == '[') { *pp = p + 1; return UC_LBRACK; }
    if (*p == ']') { *pp = p + 1; return UC_RBRACK; }
    const char *start = p;
    while (*p && !uc_is_space(*p) && *p != '[' && *p != ']' && *p != ';') p++;
    *ts = start;
    *te = p;
    *pp = p;
    return UC_ATOM;
}

/* Copy the atom [ts,te) into `name` (truncating to fit) and enable it at
 * XF_SRC_USER_CONFIG.  Returns experiment_enable's result; on false, `name`
 * holds the offending token for the caller's error message. */
static bool uc_try_enable(const char *ts, const char *te,
                          char *name, size_t namesz) {
    size_t len = (size_t)(te - ts);
    if (len >= namesz) len = namesz - 1;
    memcpy(name, ts, len);
    name[len] = '\0';
    return experiment_enable(name, XF_SRC_USER_CONFIG);
}

bool experiments_read_user_config(void) {
    char path[4096];
    if (!uc_config_path(path, sizeof(path))) return false;
    char *src = uc_slurp(path);
    if (!src) return false;   /* absent / unreadable -> no-op */

    const char *p = src;
    const char *ts, *te;
    UcTok t;
    while ((t = uc_next(&p, &ts, &te)) != UC_EOF) {
        if (t != UC_ATOM || ts[0] != ':') continue;  /* expect a :key; skip stray tokens */

        /* Copy the key name (without the leading ':'). */
        char key[128];
        size_t klen = (size_t)(te - ts) - 1;
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, ts + 1, klen);
        key[klen] = '\0';

        if (strcmp(key, "enable") == 0) {
            const char *save = p;
            t = uc_next(&p, &ts, &te);
            if (t == UC_LBRACK) {
                char name[128];
                while ((t = uc_next(&p, &ts, &te)) == UC_ATOM) {
                    if (!uc_try_enable(ts, te, name, sizeof(name))) {
                        fprintf(stderr,
                                "error [TUR-E0310]: unknown experiment '%s' in "
                                "%s :enable; run 'tur experiments' for the list\n",
                                name, path);
                        free(src);
                        exit(2);
                    }
                }
                /* t is UC_RBRACK or UC_EOF -- either way the list is done. */
            } else if (t == UC_ATOM) {
                /* Bare `:enable name` (no vector) -- accept the single name. */
                char name[128];
                if (!uc_try_enable(ts, te, name, sizeof(name))) {
                    fprintf(stderr,
                            "error [TUR-E0310]: unknown experiment '%s' in "
                            "%s :enable; run 'tur experiments' for the list\n",
                            name, path);
                    free(src);
                    exit(2);
                }
            } else {
                p = save;  /* nothing followed -- let the outer loop resync */
            }
        } else {
            /* Unknown key: warn (TUR-W0062) and skip its value so we resync. */
            fprintf(stderr,
                    "warning [TUR-W0062]: unknown key ':%s' in %s\n", key, path);
            const char *save = p;
            t = uc_next(&p, &ts, &te);
            if (t == UC_LBRACK) {
                int depth = 1;
                while (depth > 0) {
                    t = uc_next(&p, &ts, &te);
                    if (t == UC_EOF) break;
                    if (t == UC_LBRACK) depth++;
                    else if (t == UC_RBRACK) depth--;
                }
            } else if (t == UC_RBRACK || t == UC_EOF) {
                p = save;  /* not our value -- resync at the outer loop */
            }
            /* else: a single-atom value, already consumed. */
        }
    }
    free(src);
    return true;
}
