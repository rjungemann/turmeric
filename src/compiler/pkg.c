/*
 * pkg.c -- Spice: the Turmeric package manager (Phase PKG-1)
 *
 * See pkg.h for the public API.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
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

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "buf.h"
#include "diag.h"
#include "fmt.h"
#include "forms.h"
#include "pkg.h"
#include "reader.h"
#include "symbols.h"

/* ================================================================== */
/* Internal helpers                                                     */
/* ================================================================== */

/* Copy a StrSlice into a heap-allocated NUL-terminated string. */
static char *ss_dup(StrSlice s) {
    char *out = (char *)malloc(s.len + 1);
    if (!out) return NULL;
    memcpy(out, s.p, s.len);
    out[s.len] = '\0';
    return out;
}

/* Run a command and capture stdout into a heap-allocated string.
 * Returns NULL on failure.  Caller must free(). */
static char *run_capture(const char *cmd) {
    FILE *f = popen(cmd, "r");
    if (!f) return NULL;
    Buf b;
    buf_init(&b);
    char line[256];
    while (fgets(line, sizeof(line), f))
        buf_puts(&b, line);
    int rc = pclose(f);
    if (rc != 0) {
        buf_free(&b);
        return NULL;
    }
    /* NUL-terminate and strip trailing newline */
    if (b.len == 0) { buf_free(&b); return NULL; }
    buf_putc(&b, '\0');
    /* trim trailing whitespace */
    char *p = b.data + b.len - 2; /* last char before NUL */
    while (p >= b.data && (*p == '\n' || *p == '\r' || *p == ' '))
        *p-- = '\0';
    char *result = tur_strdup(b.data);
    buf_free(&b);
    return result;
}

/* Return ISO-8601 UTC timestamp in a static buffer (thread-unsafe but fine
 * here since package operations are single-threaded). */
static const char *iso_now(void) {
    static char buf[32];
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

/* Ensure a directory exists (creates intermediate parents). */
static bool mkdirp(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return true;
    /* Try to create it */
    if (mkdir(path, 0755) == 0) return true;
    if (errno == EEXIST) return true;
    /* Walk up and recurse */
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[--len] = '\0';
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            tmp[i] = '/';
        }
    }
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

/* ================================================================== */
/* Form-walking helpers for defpackage parsing                         */
/* ================================================================== */

/* Get a value from an F_MAP by keyword name (without colon).
 * Returns NULL if not found. */
static const Form *map_get_kw(const Form *map, const char *kw) {
    if (!map || map->tag != F_MAP) return NULL;
    const FormList *fl = &map->as.list;
    for (uint32_t i = 0; i + 1 < fl->len; i += 2) {
        const Form *key = fl->items[i];
        if (key->tag == F_KEYWORD && strcmp(key->as.sym->name, kw) == 0)
            return fl->items[i + 1];
    }
    return NULL;
}

/* Extract a string from an F_STR form, or NULL. */
static char *form_str_dup(const Form *f) {
    if (!f || f->tag != F_STR) return NULL;
    return ss_dup(f->as.s);
}

/* Extract a bool from an F_BOOL form. */
static bool form_bool_val(const Form *f) {
    if (!f || f->tag != F_BOOL) return false;
    return f->as.b;
}

/* Parse the :spices map: {"name" {:url "..." :ref "..."} ...} */
static bool parse_spices(const Form *map, PkgManifest *m) {
    if (!map || map->tag != F_MAP) return true; /* empty is OK */
    const FormList *fl = &map->as.list;
    int cap = 4;
    m->spices = (PkgSpice *)malloc(cap * sizeof(PkgSpice));
    if (!m->spices) return false;
    m->n_spices = 0;

    for (uint32_t i = 0; i + 1 < fl->len; i += 2) {
        const Form *key = fl->items[i];
        const Form *val = fl->items[i + 1];
        if (key->tag != F_STR) continue;
        if (!val || val->tag != F_MAP) continue;

        if (m->n_spices >= cap) {
            cap *= 2;
            m->spices = (PkgSpice *)realloc(m->spices,
                                             cap * sizeof(PkgSpice));
            if (!m->spices) return false;
        }
        PkgSpice *s = &m->spices[m->n_spices++];
        memset(s, 0, sizeof(*s));
        s->name     = ss_dup(key->as.s);
        s->url      = form_str_dup(map_get_kw(val, "url"));
        s->ref      = form_str_dup(map_get_kw(val, "ref"));
        s->path     = form_str_dup(map_get_kw(val, "path"));
        const Form *opt_f = map_get_kw(val, "optional");
        s->optional = form_bool_val(opt_f);
    }
    return true;
}

/* Forward declaration (parse_str_vec is defined after parse_cmake_deps). */
static bool parse_str_vec(const Form *f, char ***out, int *n_out);

/* Parse a single cmake dep options map: {:KEY "VAL" ...} */
static bool parse_cmake_opts(const Form *map,
                              PkgCmakeOpt **out_opts, int *out_n) {
    *out_opts = NULL;
    *out_n    = 0;
    if (!map || map->tag != F_MAP) return true;
    const FormList *fl = &map->as.list;
    int cap = 4;
    *out_opts = (PkgCmakeOpt *)malloc(cap * sizeof(PkgCmakeOpt));
    if (!*out_opts) return false;
    for (uint32_t i = 0; i + 1 < fl->len; i += 2) {
        const Form *k = fl->items[i];
        const Form *v = fl->items[i + 1];
        if (k->tag != F_KEYWORD) continue;
        if (!v || v->tag != F_STR) continue;
        if (*out_n >= cap) {
            cap *= 2;
            *out_opts = (PkgCmakeOpt *)realloc(*out_opts,
                                                cap * sizeof(PkgCmakeOpt));
            if (!*out_opts) return false;
        }
        PkgCmakeOpt *o = &(*out_opts)[(*out_n)++];
        o->key = tur_strdup(k->as.sym->name);
        o->val = ss_dup(v->as.s);
    }
    return true;
}

/* Parse the :cmake-deps map */
static bool parse_cmake_deps(const Form *map, PkgManifest *m) {
    if (!map || map->tag != F_MAP) return true;
    const FormList *fl = &map->as.list;
    int cap = 4;
    m->cmake_deps = (PkgCmakeDep *)malloc(cap * sizeof(PkgCmakeDep));
    if (!m->cmake_deps) return false;
    m->n_cmake_deps = 0;

    for (uint32_t i = 0; i + 1 < fl->len; i += 2) {
        const Form *key = fl->items[i];
        const Form *val = fl->items[i + 1];
        if (key->tag != F_STR) continue;
        if (!val || val->tag != F_MAP) continue;

        if (m->n_cmake_deps >= cap) {
            cap *= 2;
            m->cmake_deps = (PkgCmakeDep *)realloc(m->cmake_deps,
                                                     cap * sizeof(PkgCmakeDep));
            if (!m->cmake_deps) return false;
        }
        PkgCmakeDep *d = &m->cmake_deps[m->n_cmake_deps++];
        memset(d, 0, sizeof(*d));
        d->name       = ss_dup(key->as.s);
        d->url        = form_str_dup(map_get_kw(val, "url"));
        d->ref        = form_str_dup(map_get_kw(val, "ref"));
        d->path       = form_str_dup(map_get_kw(val, "path"));
        d->cmake_name = form_str_dup(map_get_kw(val, "cmake-name"));
        parse_str_vec(map_get_kw(val, "targets"), &d->targets, &d->n_targets);
        const Form *opts_f = map_get_kw(val, "options");
        parse_cmake_opts(opts_f, &d->opts, &d->n_opts);
    }
    return true;
}

/* Parse a string-vector form [a b c] or just a plain F_STR */
static bool parse_str_vec(const Form *f, char ***out, int *n_out) {
    *out   = NULL;
    *n_out = 0;
    if (!f) return true;
    if (f->tag == F_STR) {
        *out    = (char **)malloc(sizeof(char *));
        (*out)[0] = ss_dup(f->as.s);
        *n_out  = 1;
        return true;
    }
    if (f->tag != F_VEC && f->tag != F_LIST) return true;
    const FormList *fl = &f->as.list;
    *out = (char **)malloc(fl->len * sizeof(char *));
    if (!*out) return false;
    *n_out = 0;
    for (uint32_t i = 0; i < fl->len; i++) {
        if (fl->items[i]->tag == F_STR)
            (*out)[(*n_out)++] = ss_dup(fl->items[i]->as.s);
    }
    return true;
}

/* ================================================================== */
/* pkg_manifest_read                                                    */
/* ================================================================== */

bool pkg_manifest_read(const char *path, PkgManifest *out) {
    memset(out, 0, sizeof(*out));

    /* Read the file into memory */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "spice: cannot open '%s': %s\n", path, strerror(errno));
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char *)malloc(sz + 1);
    if (!src) { fclose(f); return false; }
    if (fread(src, 1, sz, f) != (size_t)sz) {
        fclose(f); free(src); return false;
    }
    fclose(f);
    src[sz] = '\0';

    /* Parse with the Turmeric reader */
    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    SourceFile file = {0};
    file.path    = path;
    file.src     = src;
    file.len     = (uint32_t)sz;
    file.file_id = 0;
    /* Use READER_TURMERIC (value 0) */
    diag_register_file(&file);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);
    free(src);

    if (!forms || diag_had_error() || nforms == 0) {
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    /* Find the (defpackage ...) form */
    Form *dp = NULL;
    for (uint32_t i = 0; i < nforms; i++) {
        Form *frm = forms[i];
        if (frm->tag != F_LIST || frm->as.list.len < 2) continue;
        Form *head = frm->as.list.items[0];
        if (head->tag == F_SYM &&
            strcmp(head->as.sym->name, "defpackage") == 0) {
            dp = frm;
            break;
        }
    }
    if (!dp) {
        fprintf(stderr, "spice: no (defpackage ...) form found in '%s'\n", path);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    /* items[1] is the package name symbol or string */
    Form *name_form = dp->as.list.items[1];
    if (name_form->tag == F_SYM)
        out->name = tur_strdup(name_form->as.sym->name);
    else if (name_form->tag == F_STR)
        out->name = ss_dup(name_form->as.s);

    /* Walk keyword-value pairs starting at index 2 */
    const FormList *fl = &dp->as.list;
    for (uint32_t i = 2; i + 1 < fl->len; i += 2) {
        const Form *kf = fl->items[i];
        const Form *vf = fl->items[i + 1];
        if (kf->tag != F_KEYWORD) { i -= 1; continue; }
        const char *kw = kf->as.sym->name;

        if (strcmp(kw, "name") == 0) {
            free(out->name);
            out->name = form_str_dup(vf);
        } else if (strcmp(kw, "version") == 0) {
            out->version = form_str_dup(vf);
        } else if (strcmp(kw, "description") == 0) {
            out->description = form_str_dup(vf);
        } else if (strcmp(kw, "license") == 0) {
            out->license = form_str_dup(vf);
        } else if (strcmp(kw, "repository") == 0) {
            out->repository = form_str_dup(vf);
        } else if (strcmp(kw, "homepage") == 0) {
            out->homepage = form_str_dup(vf);
        } else if (strcmp(kw, "authors") == 0) {
            parse_str_vec(vf, &out->authors, &out->n_authors);
        } else if (strcmp(kw, "spices") == 0) {
            parse_spices(vf, out);
        } else if (strcmp(kw, "cmake-deps") == 0) {
            parse_cmake_deps(vf, out);
        } else if (strcmp(kw, "exports") == 0) {
            parse_str_vec(vf, &out->exports, &out->n_exports);
        } else if (strcmp(kw, "build-opts") == 0) {
            if (vf && vf->tag == F_MAP) {
                const Form *cf = map_get_kw(vf, "c-flags");
                const Form *lf = map_get_kw(vf, "link-libs");
                const Form *nf = map_get_kw(vf, "no-stdlib");
                parse_str_vec(cf, &out->c_flags,   &out->n_c_flags);
                parse_str_vec(lf, &out->link_libs,  &out->n_link_libs);
                out->no_stdlib = form_bool_val(nf);
            }
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    return true;
}

/* ================================================================== */
/* pkg_manifest_write                                                   */
/* ================================================================== */

bool pkg_manifest_write(const char *path, const PkgManifest *m) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "spice: cannot write '%s': %s\n", path, strerror(errno));
        return false;
    }

    fprintf(f, ";;; build.tur -- project manifest for \"%s\"\n",
            m->name ? m->name : "unnamed");
    fprintf(f, "(defpackage %s\n", m->name ? m->name : "unnamed");
    if (m->name)        fprintf(f, "  :name        \"%s\"\n", m->name);
    if (m->version)     fprintf(f, "  :version     \"%s\"\n", m->version);
    if (m->description) fprintf(f, "  :description \"%s\"\n", m->description);
    if (m->license)     fprintf(f, "  :license     \"%s\"\n", m->license);
    if (m->repository)  fprintf(f, "  :repository  \"%s\"\n", m->repository);
    if (m->homepage)    fprintf(f, "  :homepage    \"%s\"\n", m->homepage);

    if (m->n_authors > 0) {
        fprintf(f, "  :authors     [");
        for (int i = 0; i < m->n_authors; i++) {
            if (i) fprintf(f, " ");
            fprintf(f, "\"%s\"", m->authors[i]);
        }
        fprintf(f, "]\n");
    }

    if (m->n_spices > 0) {
        fprintf(f, "\n  :spices #{\n");
        for (int i = 0; i < m->n_spices; i++) {
            const PkgSpice *s = &m->spices[i];
            if (s->path) {
                fprintf(f, "    \"%s\" #{:path \"%s\"", s->name, s->path);
            } else {
                fprintf(f, "    \"%s\" #{:url \"%s\"", s->name,
                        s->url ? s->url : "");
                if (s->ref) fprintf(f, " :ref \"%s\"", s->ref);
            }
            if (s->optional) fprintf(f, " :optional true");
            fprintf(f, "}\n");
        }
        fprintf(f, "  }\n");
    }

    if (m->n_cmake_deps > 0) {
        fprintf(f, "\n  :cmake-deps #{\n");
        for (int i = 0; i < m->n_cmake_deps; i++) {
            const PkgCmakeDep *d = &m->cmake_deps[i];
            if (d->path) {
                fprintf(f, "    \"%s\" #{:path \"%s\"", d->name, d->path);
            } else {
                fprintf(f, "    \"%s\" #{:url \"%s\"",
                        d->name, d->url ? d->url : "");
                if (d->ref) fprintf(f, " :ref \"%s\"", d->ref);
            }
            if (d->cmake_name) fprintf(f, " :cmake-name \"%s\"", d->cmake_name);
            if (d->n_targets > 0) {
                fprintf(f, " :targets [");
                for (int j = 0; j < d->n_targets; j++) {
                    if (j) fprintf(f, " ");
                    fprintf(f, "\"%s\"", d->targets[j]);
                }
                fprintf(f, "]");
            }
            if (d->n_opts > 0) {
                fprintf(f, " :options #{");
                for (int j = 0; j < d->n_opts; j++) {
                    if (j) fprintf(f, " ");
                    fprintf(f, ":%s \"%s\"",
                            d->opts[j].key, d->opts[j].val);
                }
                fprintf(f, "}");
            }
            fprintf(f, "}\n");
        }
        fprintf(f, "  }\n");
    }

    if (m->n_c_flags > 0 || m->n_link_libs > 0 || m->no_stdlib) {
        fprintf(f, "\n  :build-opts {\n");
        if (m->n_c_flags > 0) {
            fprintf(f, "    :c-flags [");
            for (int i = 0; i < m->n_c_flags; i++) {
                if (i) fprintf(f, " ");
                fprintf(f, "\"%s\"", m->c_flags[i]);
            }
            fprintf(f, "]\n");
        }
        if (m->n_link_libs > 0) {
            fprintf(f, "    :link-libs [");
            for (int i = 0; i < m->n_link_libs; i++) {
                if (i) fprintf(f, " ");
                fprintf(f, "\"%s\"", m->link_libs[i]);
            }
            fprintf(f, "]\n");
        }
        if (m->no_stdlib) fprintf(f, "    :no-stdlib true\n");
        fprintf(f, "  }\n");
    }

    if (m->n_exports > 0) {
        fprintf(f, "\n  :exports [");
        for (int i = 0; i < m->n_exports; i++) {
            if (i) fprintf(f, " ");
            fprintf(f, "\"%s\"", m->exports[i]);
        }
        fprintf(f, "]\n");
    }

    fprintf(f, ")\n");
    fclose(f);
    return true;
}

/* ================================================================== */
/* pkg_manifest_free                                                    */
/* ================================================================== */

void pkg_manifest_free(PkgManifest *m) {
    free(m->name);
    free(m->version);
    free(m->description);
    free(m->license);
    free(m->repository);
    free(m->homepage);
    for (int i = 0; i < m->n_authors; i++) free(m->authors[i]);
    free(m->authors);
    for (int i = 0; i < m->n_spices; i++) {
        free(m->spices[i].name);
        free(m->spices[i].url);
        free(m->spices[i].ref);
        free(m->spices[i].path);
    }
    free(m->spices);
    for (int i = 0; i < m->n_cmake_deps; i++) {
        free(m->cmake_deps[i].name);
        free(m->cmake_deps[i].url);
        free(m->cmake_deps[i].ref);
        free(m->cmake_deps[i].path);
        free(m->cmake_deps[i].cmake_name);
        for (int j = 0; j < m->cmake_deps[i].n_targets; j++)
            free(m->cmake_deps[i].targets[j]);
        free(m->cmake_deps[i].targets);
        for (int j = 0; j < m->cmake_deps[i].n_opts; j++) {
            free(m->cmake_deps[i].opts[j].key);
            free(m->cmake_deps[i].opts[j].val);
        }
        free(m->cmake_deps[i].opts);
    }
    free(m->cmake_deps);
    for (int i = 0; i < m->n_exports;   i++) free(m->exports[i]);
    free(m->exports);
    for (int i = 0; i < m->n_c_flags;   i++) free(m->c_flags[i]);
    free(m->c_flags);
    for (int i = 0; i < m->n_link_libs; i++) free(m->link_libs[i]);
    free(m->link_libs);
    memset(m, 0, sizeof(*m));
}

/* ================================================================== */
/* Lock file (Turmeric S-expression format)                            */
/* ================================================================== */

bool pkg_lock_read(const char *path, PkgLockFile *out) {
    memset(out, 0, sizeof(*out));
    out->format_version = 1;

    FILE *f = fopen(path, "r");
    if (!f) return false; /* not an error -- no lock file yet */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char *)malloc(sz + 1);
    if (!src) { fclose(f); return false; }
    if (fread(src, 1, sz, f) != (size_t)sz) { fclose(f); free(src); return false; }
    fclose(f);
    src[sz] = '\0';

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    SourceFile file = {0};
    file.path    = path;
    file.src     = src;
    file.len     = (uint32_t)sz;
    file.file_id = 0;
    diag_register_file(&file);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);
    free(src);

    if (!forms || diag_had_error() || nforms == 0) {
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    /* Find (deflockfile ...) */
    Form *dlf = NULL;
    for (uint32_t i = 0; i < nforms; i++) {
        Form *frm = forms[i];
        if (frm->tag != F_LIST || frm->as.list.len < 1) continue;
        Form *head = frm->as.list.items[0];
        if (head->tag == F_SYM &&
            strcmp(head->as.sym->name, "deflockfile") == 0) {
            dlf = frm;
            break;
        }
    }
    if (!dlf) {
        /* Empty or unrecognised format -- return empty lock */
        symtab_free(&st);
        arena_free(&arena);
        return true;
    }

    /* Walk keyword-value pairs starting at index 1 */
    const FormList *fl = &dlf->as.list;
    for (uint32_t i = 1; i + 1 < fl->len; i += 2) {
        const Form *kf = fl->items[i];
        const Form *vf = fl->items[i + 1];
        if (kf->tag != F_KEYWORD) { i -= 1; continue; }
        const char *kw = kf->as.sym->name;

        if (strcmp(kw, "format-version") == 0) {
            if (vf->tag == F_INT) out->format_version = (int)vf->as.i;
        } else if (strcmp(kw, "spices") == 0 ||
                   strcmp(kw, "cmake-deps") == 0) {
            bool is_cmake = (strcmp(kw, "cmake-deps") == 0);
            if (!vf || vf->tag != F_MAP) continue;
            const FormList *mfl = &vf->as.list;
            for (uint32_t j = 0; j + 1 < mfl->len; j += 2) {
                const Form *nf = mfl->items[j];
                const Form *ef = mfl->items[j + 1];
                if (nf->tag != F_STR) continue;
                if (!ef || ef->tag != F_MAP) continue;

                out->entries = (PkgLockEntry *)realloc(out->entries,
                    (out->n_entries + 1) * sizeof(PkgLockEntry));
                if (!out->entries) {
                    symtab_free(&st); arena_free(&arena); return false;
                }
                PkgLockEntry *e = &out->entries[out->n_entries++];
                memset(e, 0, sizeof(*e));
                e->name       = ss_dup(nf->as.s);
                e->is_cmake   = is_cmake;
                e->url        = form_str_dup(map_get_kw(ef, "url"));
                e->ref        = form_str_dup(map_get_kw(ef, "ref"));
                e->resolved   = form_str_dup(map_get_kw(ef, "resolved"));
                e->sha256     = form_str_dup(map_get_kw(ef, "sha256"));
                e->fetched_at = form_str_dup(map_get_kw(ef, "fetched-at"));

                /* transitive: ["name@ref" ...] */
                const Form *tr = map_get_kw(ef, "transitive");
                if (tr && (tr->tag == F_VEC || tr->tag == F_LIST)) {
                    const FormList *tfl = &tr->as.list;
                    if (tfl->len > 0) {
                        e->transitive = (char **)malloc(
                            tfl->len * sizeof(char *));
                        for (uint32_t k = 0; k < tfl->len; k++) {
                            if (tfl->items[k]->tag == F_STR)
                                e->transitive[e->n_transitive++] =
                                    ss_dup(tfl->items[k]->as.s);
                        }
                    }
                }
            }
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    return true;
}

bool pkg_lock_write(const char *path, const PkgLockFile *lock) {
    /* Write to a temp file then rename for atomicity */
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        fprintf(stderr, "spice: cannot write lock file '%s': %s\n",
                path, strerror(errno));
        return false;
    }

    fprintf(f, ";;; tur.lock -- generated by tur. Do not edit by hand.\n");
    fprintf(f, ";;; Commit this file to version control for reproducible builds.\n\n");
    fprintf(f, "(deflockfile\n");
    fprintf(f, "  :format-version %d\n", lock->format_version);

    /* Spices section -- use #{} map syntax (standard Turmeric, no #lang needed) */
    fprintf(f, "  :spices #{\n");
    for (int i = 0; i < lock->n_entries; i++) {
        const PkgLockEntry *e = &lock->entries[i];
        if (e->is_cmake) continue;
        fprintf(f, "    \"%s\" #{", e->name);
        if (e->url)        fprintf(f, ":url \"%s\" ", e->url);
        if (e->ref)        fprintf(f, ":ref \"%s\" ", e->ref);
        if (e->resolved)   fprintf(f, ":resolved \"%s\" ", e->resolved);
        if (e->sha256)     fprintf(f, ":sha256 \"%s\" ", e->sha256);
        if (e->fetched_at) fprintf(f, ":fetched-at \"%s\" ", e->fetched_at);
        if (e->n_transitive > 0) {
            fprintf(f, ":transitive [");
            for (int j = 0; j < e->n_transitive; j++) {
                if (j) fprintf(f, " ");
                fprintf(f, "\"%s\"", e->transitive[j]);
            }
            fprintf(f, "] ");
        }
        fprintf(f, "}\n");
    }
    fprintf(f, "  }\n");

    /* cmake-deps section */
    fprintf(f, "  :cmake-deps #{\n");
    for (int i = 0; i < lock->n_entries; i++) {
        const PkgLockEntry *e = &lock->entries[i];
        if (!e->is_cmake) continue;
        fprintf(f, "    \"%s\" #{", e->name);
        if (e->url)        fprintf(f, ":url \"%s\" ", e->url);
        if (e->ref)        fprintf(f, ":ref \"%s\" ", e->ref);
        if (e->resolved)   fprintf(f, ":resolved \"%s\" ", e->resolved);
        if (e->sha256)     fprintf(f, ":sha256 \"%s\" ", e->sha256);
        if (e->fetched_at) fprintf(f, ":fetched-at \"%s\" ", e->fetched_at);
        fprintf(f, "}\n");
    }
    fprintf(f, "  })\n");

    fclose(f);
    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "spice: rename failed: %s\n", strerror(errno));
        unlink(tmp_path);
        return false;
    }
    return true;
}

void pkg_lock_free(PkgLockFile *lock) {
    for (int i = 0; i < lock->n_entries; i++) {
        PkgLockEntry *e = &lock->entries[i];
        free(e->name);
        free(e->url);
        free(e->ref);
        free(e->resolved);
        free(e->sha256);
        free(e->fetched_at);
        for (int j = 0; j < e->n_transitive; j++) free(e->transitive[j]);
        free(e->transitive);
    }
    free(lock->entries);
    memset(lock, 0, sizeof(*lock));
}

PkgLockEntry *pkg_lock_find(PkgLockFile *lock,
                             const char *name, bool is_cmake) {
    for (int i = 0; i < lock->n_entries; i++) {
        PkgLockEntry *e = &lock->entries[i];
        if (e->is_cmake == is_cmake && strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

/* Add (or update) an entry in the lock file. */
static PkgLockEntry *lock_upsert(PkgLockFile *lock,
                                  const char *name, bool is_cmake) {
    PkgLockEntry *e = pkg_lock_find(lock, name, is_cmake);
    if (e) return e;
    /* append */
    lock->entries = (PkgLockEntry *)realloc(lock->entries,
        (lock->n_entries + 1) * sizeof(PkgLockEntry));
    if (!lock->entries) return NULL;
    e = &lock->entries[lock->n_entries++];
    memset(e, 0, sizeof(*e));
    e->name     = tur_strdup(name);
    e->is_cmake = is_cmake;
    return e;
}

/* ================================================================== */
/* SHA-256                                                              */
/* ================================================================== */

bool pkg_sha256_file(const char *path, char out[65]) {
    Buf cmd;
    buf_init(&cmd);
#if defined(__APPLE__)
    buf_printf(&cmd, "shasum -a 256 '%s'", path);
#else
    buf_printf(&cmd, "sha256sum '%s'", path);
#endif
    buf_putc(&cmd, '\0');
    FILE *f = popen(cmd.data, "r");
    buf_free(&cmd);
    if (!f) return false;
    char line[256];
    bool ok = false;
    if (fgets(line, sizeof(line), f)) {
        /* output: "<64 hex chars>  <filename>" */
        if (strlen(line) >= 64) {
            memcpy(out, line, 64);
            out[64] = '\0';
            ok = true;
        }
    }
    pclose(f);
    return ok;
}

bool pkg_sha256_dir(const char *dir, char out[65]) {
    Buf cmd;
    buf_init(&cmd);
#if defined(__APPLE__)
    buf_printf(&cmd, "tar -c '%s' 2>/dev/null | shasum -a 256", dir);
#else
    buf_printf(&cmd, "tar -c '%s' 2>/dev/null | sha256sum", dir);
#endif
    buf_putc(&cmd, '\0');
    FILE *f = popen(cmd.data, "r");
    buf_free(&cmd);
    if (!f) return false;
    char line[256];
    bool ok = false;
    if (fgets(line, sizeof(line), f)) {
        if (strlen(line) >= 64) {
            memcpy(out, line, 64);
            out[64] = '\0';
            ok = true;
        }
    }
    pclose(f);
    return ok;
}

/* ================================================================== */
/* Semver                                                               */
/* ================================================================== */

bool pkg_semver_parse(const char *v,
                      int *major, int *minor, int *patch,
                      char **pre) {
    if (!v) return false;
    if (*v == 'v') v++;
    /* parse major.minor.patch */
    char *end;
    long ma = strtol(v, &end, 10);
    if (*end != '.') return false;
    v = end + 1;
    long mi = strtol(v, &end, 10);
    if (*end != '.' && *end != '\0' && *end != '-') return false;
    v = (*end == '.') ? end + 1 : end;
    long pa = 0;
    if (*end == '.') {
        pa = strtol(v, &end, 10);
    }
    *major = (int)ma;
    *minor = (int)mi;
    *patch = (int)pa;
    if (pre) {
        if (*end == '-')
            *pre = tur_strdup(end + 1);
        else
            *pre = NULL;
    }
    return true;
}

int pkg_semver_compare(const char *a, const char *b) {
    int ma, mi, pa, mb, mib, pb;
    char *prea = NULL, *preb = NULL;
    bool oka = pkg_semver_parse(a, &ma, &mi, &pa, &prea);
    bool okb = pkg_semver_parse(b, &mb, &mib, &pb, &preb);
    if (!oka && !okb) { free(prea); free(preb); return strcmp(a, b); }
    if (!oka) { free(prea); free(preb); return -1; }
    if (!okb) { free(prea); free(preb); return  1; }
    int d = (ma != mb) ? ma - mb : (mi != mib) ? mi - mib : pa - pb;
    free(prea);
    free(preb);
    return d;
}

/* ================================================================== */
/* Git operations                                                       */
/* ================================================================== */

char *pkg_git_resolve(const char *repo_dir) {
    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "git -C '%s' rev-parse HEAD 2>/dev/null", repo_dir);
    buf_putc(&cmd, '\0');
    char *sha = run_capture(cmd.data);
    buf_free(&cmd);
    return sha;
}

char *pkg_git_fetch(const char *url, const char *ref, const char *dest_dir) {
    struct stat st;
    bool already_cloned = (stat(dest_dir, &st) == 0 && S_ISDIR(st.st_mode));

    Buf cmd;
    buf_init(&cmd);

    if (!already_cloned) {
        /* Fresh clone */
        if (ref) {
            buf_printf(&cmd,
                "git clone --depth 1 --branch '%s' -- '%s' '%s' 2>&1",
                ref, url, dest_dir);
        } else {
            buf_printf(&cmd,
                "git clone --depth 1 -- '%s' '%s' 2>&1",
                url, dest_dir);
        }
    } else {
        /* Fetch and checkout the desired ref */
        buf_printf(&cmd,
            "git -C '%s' fetch --depth 1 origin '%s' 2>&1 && "
            "git -C '%s' checkout FETCH_HEAD 2>&1",
            dest_dir, ref ? ref : "HEAD",
            dest_dir);
    }
    buf_putc(&cmd, '\0');
    int rc = system(cmd.data);
    buf_free(&cmd);

    if (rc != 0) {
        fprintf(stderr, "spice: git failed for '%s' ref '%s'\n",
                url, ref ? ref : "(default)");
        return NULL;
    }
    return pkg_git_resolve(dest_dir);
}

/* ================================================================== */
/* pkg_fetch_all -- BFS transitive resolution                          */
/* ================================================================== */

/* A pending fetch item */
typedef struct FetchItem {
    char *name;
    char *url;
    char *ref;
    char *path;  /* NULL = git dep */
    bool  is_cmake;
    /* origin for error reporting */
    char *from;
} FetchItem;

/* Conflict table entry */
typedef struct ConflictEntry {
    char *name;
    char *url;
    char *ref;
} ConflictEntry;

bool pkg_fetch_all(const char *project_dir,
                   const PkgManifest *manifest,
                   PkgLockFile *lock,
                   bool update) {
    /* Create spices/ directory */
    char spices_dir[4096];
    snprintf(spices_dir, sizeof(spices_dir), "%s/spices", project_dir);
    if (!mkdirp(spices_dir)) {
        fprintf(stderr, "spice: cannot create '%s': %s\n",
                spices_dir, strerror(errno));
        return false;
    }

    /* BFS queue */
    int q_cap = 32;
    int q_len = 0;
    int q_head = 0;
    FetchItem *queue = (FetchItem *)malloc(q_cap * sizeof(FetchItem));
    if (!queue) return false;

    /* Conflict table (name → url@ref) */
    int cf_cap = 16;
    int n_cf   = 0;
    ConflictEntry *conflicts = (ConflictEntry *)malloc(cf_cap * sizeof(ConflictEntry));
    if (!conflicts) { free(queue); return false; }

    /* Seed the queue from the manifest's direct spices */
    for (int i = 0; i < manifest->n_spices; i++) {
        const PkgSpice *s = &manifest->spices[i];
        if (q_len >= q_cap) {
            q_cap *= 2;
            queue = (FetchItem *)realloc(queue, q_cap * sizeof(FetchItem));
            if (!queue) return false;
        }
        FetchItem *it = &queue[q_len++];
        it->name     = tur_strdup(s->name);
        it->url      = s->url  ? tur_strdup(s->url)  : NULL;
        it->ref      = s->ref  ? tur_strdup(s->ref)  : NULL;
        it->path     = s->path ? tur_strdup(s->path) : NULL;
        it->is_cmake = false;
        it->from     = tur_strdup("(root)");
    }

    bool ok = true;

    while (q_head < q_len) {
        FetchItem *it = &queue[q_head++];

        if (it->is_cmake) {
            /* cmake deps are handled in pkg_gen_cmake_deps, not fetched here */
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->from);
            continue;
        }

        /* Local path dep — no fetch needed, just record */
        if (it->path) {
            PkgLockEntry *le = lock_upsert(lock, it->name, false);
            if (le) {
                free(le->url); le->url = NULL;
                free(le->ref); le->ref = NULL;
            }
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->from);
            continue;
        }

        /* Check for conflicts with previously seen deps */
        bool conflict = false;
        for (int c = 0; c < n_cf; c++) {
            if (strcmp(conflicts[c].name, it->name) == 0) {
                /* Same name — check if same url+ref */
                bool same_url = (!conflicts[c].url && !it->url) ||
                                (conflicts[c].url && it->url &&
                                 strcmp(conflicts[c].url, it->url) == 0);
                bool same_ref = (!conflicts[c].ref && !it->ref) ||
                                (conflicts[c].ref && it->ref &&
                                 strcmp(conflicts[c].ref, it->ref) == 0);
                if (!same_url || !same_ref) {
                    fprintf(stderr,
                        "spice: conflict! '%s' required as '%s@%s' "
                        "(from %s) but already resolved as '%s@%s'\n",
                        it->name,
                        it->url ? it->url : "(local)",
                        it->ref ? it->ref : "(default)",
                        it->from,
                        conflicts[c].url ? conflicts[c].url : "(local)",
                        conflicts[c].ref ? conflicts[c].ref : "(default)");
                    ok = false;
                }
                conflict = true;
                break;
            }
        }
        if (conflict) {
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->from);
            continue;
        }

        /* Register in conflict table */
        if (n_cf >= cf_cap) {
            cf_cap *= 2;
            conflicts = (ConflictEntry *)realloc(conflicts,
                cf_cap * sizeof(ConflictEntry));
            if (!conflicts) { ok = false; break; }
        }
        ConflictEntry *ce = &conflicts[n_cf++];
        ce->name = tur_strdup(it->name);
        ce->url  = it->url ? tur_strdup(it->url) : NULL;
        ce->ref  = it->ref ? tur_strdup(it->ref) : NULL;

        /* Check if already in lock (skip if not --update) */
        PkgLockEntry *le = pkg_lock_find(lock, it->name, false);
        if (le && !update) {
            fprintf(stderr, "spice: using cached '%s' @ %s\n",
                    it->name, le->resolved ? le->resolved : le->ref);
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->from);
            continue;
        }

        /* Build dest path: spices/<name>-<ref> or spices/<name> */
        char dest[4096];
        if (it->ref)
            snprintf(dest, sizeof(dest), "%s/%s-%s",
                     spices_dir, it->name, it->ref);
        else
            snprintf(dest, sizeof(dest), "%s/%s", spices_dir, it->name);

        fprintf(stderr, "spice: fetching '%s' from %s (ref: %s) ...\n",
                it->name, it->url, it->ref ? it->ref : "(default)");

        char *resolved = pkg_git_fetch(it->url, it->ref, dest);
        if (!resolved) {
            fprintf(stderr, "spice: failed to fetch '%s'\n", it->name);
            ok = false;
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->from);
            continue;
        }

        /* Update lock entry */
        le = lock_upsert(lock, it->name, false);
        if (le) {
            free(le->url);       le->url       = it->url ? tur_strdup(it->url) : NULL;
            free(le->ref);       le->ref       = it->ref ? tur_strdup(it->ref) : NULL;
            free(le->resolved);  le->resolved  = resolved;
            free(le->fetched_at);le->fetched_at= tur_strdup(iso_now());
            /* Compute SHA-256 of the fetched directory archive */
            char dir_sha[65];
            free(le->sha256);
            if (pkg_sha256_dir(dest, dir_sha))
                le->sha256 = tur_strdup(dir_sha);
            else
                le->sha256 = tur_strdup(resolved); /* fallback: git SHA */
        } else {
            free(resolved);
        }

        /* Read transitive deps from this spice's build.tur */
        char sub_build[4096];
        snprintf(sub_build, sizeof(sub_build), "%s/build.tur", dest);
        struct stat sub_st;
        if (stat(sub_build, &sub_st) == 0) {
            PkgManifest sub;
            if (pkg_manifest_read(sub_build, &sub)) {
                /* Record transitive deps in lock entry */
                if (le) {
                    for (int j = 0; j < le->n_transitive; j++)
                        free(le->transitive[j]);
                    free(le->transitive);
                    le->transitive   = NULL;
                    le->n_transitive = 0;
                    int tr_cap = sub.n_spices;
                    if (tr_cap > 0)
                        le->transitive = (char **)malloc(tr_cap * sizeof(char *));
                }
                /* Enqueue sub-spices */
                for (int j = 0; j < sub.n_spices; j++) {
                    const PkgSpice *ss = &sub.spices[j];
                    if (q_len >= q_cap) {
                        q_cap *= 2;
                        queue = (FetchItem *)realloc(queue,
                            q_cap * sizeof(FetchItem));
                        if (!queue) { ok = false; break; }
                    }
                    FetchItem *nit = &queue[q_len++];
                    nit->name     = tur_strdup(ss->name);
                    nit->url      = ss->url  ? tur_strdup(ss->url)  : NULL;
                    nit->ref      = ss->ref  ? tur_strdup(ss->ref)  : NULL;
                    nit->path     = ss->path ? tur_strdup(ss->path) : NULL;
                    nit->is_cmake = false;
                    char from_buf[256];
                    snprintf(from_buf, sizeof(from_buf), "%s@%s",
                             it->name, it->ref ? it->ref : "HEAD");
                    nit->from = tur_strdup(from_buf);

                    /* record in transitive list */
                    if (le && le->transitive) {
                        char tr_str[512];
                        snprintf(tr_str, sizeof(tr_str), "%s@%s",
                                 ss->name, ss->ref ? ss->ref : "HEAD");
                        le->transitive[le->n_transitive++] = tur_strdup(tr_str);
                    }
                }
                pkg_manifest_free(&sub);
            }
        }

        free(it->name); free(it->url); free(it->ref);
        free(it->path); free(it->from);
    }

    /* Free remaining unprocessed items */
    while (q_head < q_len) {
        FetchItem *it = &queue[q_head++];
        free(it->name); free(it->url); free(it->ref);
        free(it->path); free(it->from);
    }
    free(queue);

    /* Free conflict table */
    for (int i = 0; i < n_cf; i++) {
        free(conflicts[i].name);
        free(conflicts[i].url);
        free(conflicts[i].ref);
    }
    free(conflicts);

    return ok;
}

/* ================================================================== */
/* CMake dependency file generation                                    */
/* ================================================================== */

/* Return the link-lib name to use for a cmake dep.
 * Rules (in order):
 *   1. If :targets is set, use the last component of the first target after "::".
 *   2. If :cmake-name is set, use cmake-name.
 *   3. Use the dep name.
 * The returned pointer points into existing strings; do not free. */
static const char *cmake_dep_link_lib(const PkgCmakeDep *d) {
    if (d->n_targets > 0) {
        const char *t = d->targets[0];
        const char *sep = strrchr(t, ':');
        return (sep && sep[1]) ? sep + 1 : t;
    }
    if (d->cmake_name) return d->cmake_name;
    return d->name;
}

bool pkg_gen_cmake_deps(const char *project_dir,
                        const PkgManifest *manifest) {
    if (manifest->n_cmake_deps == 0) return true;

    char cmake_dir[4096];
    snprintf(cmake_dir, sizeof(cmake_dir), "%s/cmake", project_dir);
    if (!mkdirp(cmake_dir)) {
        fprintf(stderr, "spice: cannot create '%s'\n", cmake_dir);
        return false;
    }

    /* Write cmake/CMakeLists.txt (the entrypoint for cmake -S cmake/) */
    char out_path[4096];
    snprintf(out_path, sizeof(out_path), "%s/CMakeLists.txt", cmake_dir);
    FILE *f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "spice: cannot write '%s': %s\n",
                out_path, strerror(errno));
        return false;
    }

    fprintf(f, "# cmake/CMakeLists.txt -- AUTO-GENERATED by `tur fetch`. Do not edit.\n");
    fprintf(f, "cmake_minimum_required(VERSION 3.20)\n");
    fprintf(f, "project(SpiceDeps)\n\n");
    fprintf(f, "include(FetchContent)\n\n");

    /* Declare and make available each dep */
    for (int i = 0; i < manifest->n_cmake_deps; i++) {
        const PkgCmakeDep *d = &manifest->cmake_deps[i];
        fprintf(f, "# %s\n", d->name);
        if (d->path) {
            /* Local path dep -- use add_subdirectory with absolute path */
            char abs_path[4096];
            snprintf(abs_path, sizeof(abs_path), "%s/%s", project_dir, d->path);
            char build_subdir[4096];
            snprintf(build_subdir, sizeof(build_subdir),
                     "${CMAKE_BINARY_DIR}/_local/%s-build", d->name);
            fprintf(f, "add_subdirectory(\"%s\" \"%s\")\n\n",
                    abs_path, build_subdir);
        } else {
            fprintf(f, "FetchContent_Declare(%s\n", d->name);
            if (d->url) fprintf(f, "  GIT_REPOSITORY %s\n", d->url);
            if (d->ref) fprintf(f, "  GIT_TAG        %s\n", d->ref);
            fprintf(f, ")\n");
            for (int j = 0; j < d->n_opts; j++) {
                fprintf(f, "set(%s %s CACHE BOOL \"\" FORCE)\n",
                        d->opts[j].key, d->opts[j].val);
            }
            fprintf(f, "FetchContent_MakeAvailable(%s)\n\n", d->name);
        }
    }

    /* Generate spice-deps-manifest.json at cmake configure time */
    fprintf(f, "# --- Generate spice-deps-manifest.json ---\n");
    fprintf(f, "set(_spice_manifest_path "
               "\"${CMAKE_CURRENT_SOURCE_DIR}/spice-deps-manifest.json\")\n");
    fprintf(f, "set(_spice_manifest \"{\\n\")\n");
    fprintf(f, "set(_spice_first TRUE)\n\n");

    for (int i = 0; i < manifest->n_cmake_deps; i++) {
        const PkgCmakeDep *d = &manifest->cmake_deps[i];
        const char *link_lib = cmake_dep_link_lib(d);

        fprintf(f, "# manifest entry: %s\n", d->name);
        fprintf(f, "if(NOT _spice_first)\n");
        fprintf(f, "  string(APPEND _spice_manifest \",\\n\")\n");
        fprintf(f, "endif()\n");
        fprintf(f, "set(_spice_first FALSE)\n");

        if (d->path) {
            /* Local path dep: derive paths from the path field */
            char abs_path[4096];
            snprintf(abs_path, sizeof(abs_path), "%s/%s", project_dir, d->path);
            fprintf(f, "set(_spice_%s_inc \"%s/include\")\n",
                    d->name, abs_path);
            fprintf(f, "if(NOT EXISTS \"${_spice_%s_inc}\")\n", d->name);
            fprintf(f, "  set(_spice_%s_inc \"%s\")\n", d->name, abs_path);
            fprintf(f, "endif()\n");
            fprintf(f, "set(_spice_%s_bld "
                       "\"${CMAKE_BINARY_DIR}/_local/%s-build\")\n",
                    d->name, d->name);
        } else {
            fprintf(f, "FetchContent_GetProperties(%s)\n", d->name);
            fprintf(f, "if(EXISTS \"${%s_SOURCE_DIR}/include\")\n", d->name);
            fprintf(f, "  set(_spice_%s_inc \"${%s_SOURCE_DIR}/include\")\n",
                    d->name, d->name);
            fprintf(f, "else()\n");
            fprintf(f, "  set(_spice_%s_inc \"${%s_SOURCE_DIR}\")\n",
                    d->name, d->name);
            fprintf(f, "endif()\n");
            fprintf(f, "set(_spice_%s_bld \"${%s_BINARY_DIR}\")\n",
                    d->name, d->name);
        }

        fprintf(f, "string(APPEND _spice_manifest\n");
        fprintf(f, "  \"  \\\"%s\\\": {\\n\"\n", d->name);
        fprintf(f, "  \"    \\\"include_dirs\\\": [\\\"${_spice_%s_inc}\\\"],\\n\"\n",
                d->name);
        fprintf(f, "  \"    \\\"link_dirs\\\":    [\\\"${_spice_%s_bld}\\\"],\\n\"\n",
                d->name);
        fprintf(f, "  \"    \\\"link_libs\\\":    [\\\"%s\\\"]\\n\"\n", link_lib);
        fprintf(f, "  \"  }\")\n\n");
    }

    fprintf(f, "string(APPEND _spice_manifest \"\\n}\\n\")\n");
    fprintf(f, "file(WRITE \"${_spice_manifest_path}\" \"${_spice_manifest}\")\n");
    fprintf(f, "message(STATUS \"spice: wrote cmake/spice-deps-manifest.json\")\n");

    fclose(f);
    fprintf(stderr, "spice: generated %s\n", out_path);
    return true;
}

/* ================================================================== */
/* cmake build invocation                                              */
/* ================================================================== */

bool pkg_cmake_build(const char *project_dir,
                     const PkgManifest *manifest,
                     PkgLockFile *lock) {
    if (manifest->n_cmake_deps == 0) return true;

    char cmake_src[4096];
    snprintf(cmake_src, sizeof(cmake_src), "%s/cmake", project_dir);

    char cmake_bld[4096];
    snprintf(cmake_bld, sizeof(cmake_bld), "%s/cmake/build", project_dir);

    if (!mkdirp(cmake_bld)) {
        fprintf(stderr, "spice: cannot create '%s'\n", cmake_bld);
        return false;
    }

    /* Configure */
    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "cmake -S '%s' -B '%s'", cmake_src, cmake_bld);
    buf_putc(&cmd, '\0');
    fprintf(stderr, "spice: cmake configure ...\n");
    int rc = system(cmd.data);
    buf_free(&cmd);
    if (rc != 0) {
        fprintf(stderr, "spice: cmake configure failed (status %d)\n", rc);
        return false;
    }

    /* Build */
    buf_init(&cmd);
    buf_printf(&cmd, "cmake --build '%s'", cmake_bld);
    buf_putc(&cmd, '\0');
    fprintf(stderr, "spice: cmake build ...\n");
    rc = system(cmd.data);
    buf_free(&cmd);
    if (rc != 0) {
        fprintf(stderr, "spice: cmake build failed (status %d)\n", rc);
        return false;
    }

    /* Update tur.lock cmake-dep entries with resolved git SHAs */
    for (int i = 0; i < manifest->n_cmake_deps; i++) {
        const PkgCmakeDep *d = &manifest->cmake_deps[i];
        if (!d->url) continue; /* local path deps are not locked */

        PkgLockEntry *le = lock_upsert(lock, d->name, true);
        if (!le) continue;

        free(le->url); le->url = tur_strdup(d->url);
        free(le->ref); le->ref = d->ref ? tur_strdup(d->ref) : NULL;
        free(le->fetched_at); le->fetched_at = tur_strdup(iso_now());

        /* Read git HEAD SHA from cmake's fetched source directory */
        char dep_src[4096];
        snprintf(dep_src, sizeof(dep_src), "%s/_deps/%s-src", cmake_bld, d->name);
        char *sha = pkg_git_resolve(dep_src);
        if (sha) {
            free(le->resolved); le->resolved = sha;
            free(le->sha256);   le->sha256   = tur_strdup(sha);
        }
    }

    return true;
}

bool pkg_cmake_verify_lock(const char *project_dir,
                            const PkgLockFile *lock) {
    char cmake_bld[4096];
    snprintf(cmake_bld, sizeof(cmake_bld), "%s/cmake/build", project_dir);

    bool ok = true;
    for (int i = 0; i < lock->n_entries; i++) {
        const PkgLockEntry *le = &lock->entries[i];
        if (!le->is_cmake || !le->url || !le->resolved) continue;

        char dep_src[4096];
        snprintf(dep_src, sizeof(dep_src), "%s/_deps/%s-src", cmake_bld, le->name);

        struct stat st;
        if (stat(dep_src, &st) != 0) continue; /* not yet fetched -- skip */

        char *sha = pkg_git_resolve(dep_src);
        if (!sha) continue;

        if (strcmp(sha, le->resolved) != 0) {
            fprintf(stderr,
                "spice: cmake dep '%s' SHA mismatch!\n"
                "  lock: %s\n"
                "  disk: %s\n"
                "  Run `tur fetch` to resync.\n",
                le->name, le->resolved, sha);
            ok = false;
        }
        free(sha);
    }
    return ok;
}

/* ================================================================== */
/* spice-deps-manifest.json parsing                                    */
/* ================================================================== */

static const char *json_skip_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Parse a quoted JSON string. Advances *pp past the closing quote.
 * Returns heap-allocated C string, or NULL on parse error. */
static char *json_parse_str(const char **pp) {
    const char *p = json_skip_ws(*pp);
    if (!p || *p != '"') return NULL;
    p++;
    const char *s = p;
    while (*p && *p != '"') {
        if (*p == '\\') { p++; if (*p) p++; } /* skip escaped char */
        else p++;
    }
    if (*p != '"') return NULL;
    size_t n = (size_t)(p - s);
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    *pp = p + 1;
    return r;
}

/* Parse a JSON array of strings ["a","b",...].
 * Advances *pp past the closing ]. */
static char **json_parse_str_arr(const char **pp, int *n_out) {
    *n_out = 0;
    const char *p = json_skip_ws(*pp);
    if (!p || *p != '[') return NULL;
    p++;
    int cap = 4;
    char **arr = (char **)malloc(cap * sizeof(char *));
    if (!arr) return NULL;
    while (1) {
        p = json_skip_ws(p);
        if (!*p || *p == ']') break;
        if (*p == ',') { p++; continue; }
        char *s = json_parse_str(&p);
        if (!s) { if (*p) p++; continue; }
        if (*n_out >= cap) {
            cap *= 2;
            arr = (char **)realloc(arr, cap * sizeof(char *));
            if (!arr) { free(s); return NULL; }
        }
        arr[(*n_out)++] = s;
    }
    if (*p == ']') p++;
    *pp = p;
    return arr;
}

bool pkg_cmake_manifest_read(const char *path, PkgCmakeManifest *out) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "r");
    if (!f) return true; /* not present is not an error */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char *)malloc((size_t)sz + 1);
    if (!src) { fclose(f); return false; }
    if (fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(src); return false;
    }
    fclose(f);
    src[sz] = '\0';

    const char *p = src;
    p = json_skip_ws(p);
    if (*p != '{') { free(src); return false; }
    p++;

    int cap = 8;
    out->entries = (PkgCmakeManifestEntry *)malloc(
        cap * sizeof(PkgCmakeManifestEntry));
    if (!out->entries) { free(src); return false; }

    while (1) {
        p = json_skip_ws(p);
        if (!*p || *p == '}') break;
        if (*p == ',') { p++; continue; }

        /* Parse dep name */
        char *name = json_parse_str(&p);
        if (!name) { if (*p) p++; continue; }

        p = json_skip_ws(p);
        if (*p != ':') { free(name); if (*p) p++; continue; }
        p++;
        p = json_skip_ws(p);
        if (*p != '{') { free(name); if (*p) p++; continue; }
        p++;

        if (out->n_entries >= cap) {
            cap *= 2;
            out->entries = (PkgCmakeManifestEntry *)realloc(
                out->entries, cap * sizeof(PkgCmakeManifestEntry));
            if (!out->entries) { free(name); free(src); return false; }
        }
        PkgCmakeManifestEntry *e = &out->entries[out->n_entries++];
        memset(e, 0, sizeof(*e));
        e->name = name;

        /* Parse the entry object */
        while (1) {
            p = json_skip_ws(p);
            if (!*p || *p == '}') break;
            if (*p == ',') { p++; continue; }

            char *key = json_parse_str(&p);
            if (!key) { if (*p) p++; continue; }
            p = json_skip_ws(p);
            if (*p != ':') { free(key); if (*p) p++; continue; }
            p++;

            if (strcmp(key, "include_dirs") == 0) {
                e->include_dirs = json_parse_str_arr(&p, &e->n_include_dirs);
            } else if (strcmp(key, "link_dirs") == 0) {
                e->link_dirs = json_parse_str_arr(&p, &e->n_link_dirs);
            } else if (strcmp(key, "link_libs") == 0) {
                e->link_libs = json_parse_str_arr(&p, &e->n_link_libs);
            } else {
                /* skip unknown value */
                p = json_skip_ws(p);
                if (*p == '"') { char *tmp = json_parse_str(&p); free(tmp); }
                else if (*p == '[') {
                    int dummy = 0;
                    char **tmp = json_parse_str_arr(&p, &dummy);
                    for (int k = 0; k < dummy; k++) free(tmp[k]);
                    free(tmp);
                } else {
                    while (*p && *p != ',' && *p != '}') p++;
                }
            }
            free(key);
        }
        if (*p == '}') p++;
    }

    free(src);
    return true;
}

void pkg_cmake_manifest_free(PkgCmakeManifest *m) {
    for (int i = 0; i < m->n_entries; i++) {
        PkgCmakeManifestEntry *e = &m->entries[i];
        free(e->name);
        for (int j = 0; j < e->n_include_dirs; j++) free(e->include_dirs[j]);
        free(e->include_dirs);
        for (int j = 0; j < e->n_link_dirs; j++) free(e->link_dirs[j]);
        free(e->link_dirs);
        for (int j = 0; j < e->n_link_libs; j++) free(e->link_libs[j]);
        free(e->link_libs);
    }
    free(m->entries);
    memset(m, 0, sizeof(*m));
}

void pkg_cmake_manifest_append_cc_flags(const PkgCmakeManifest *m, Buf *buf) {
    for (int i = 0; i < m->n_entries; i++) {
        const PkgCmakeManifestEntry *e = &m->entries[i];
        for (int j = 0; j < e->n_include_dirs; j++) {
            if (e->include_dirs[j] && e->include_dirs[j][0])
                buf_printf(buf, " -I%s", e->include_dirs[j]);
        }
        for (int j = 0; j < e->n_link_dirs; j++) {
            if (e->link_dirs[j] && e->link_dirs[j][0])
                buf_printf(buf, " -L%s", e->link_dirs[j]);
        }
        for (int j = 0; j < e->n_link_libs; j++) {
            if (e->link_libs[j] && e->link_libs[j][0])
                buf_printf(buf, " -l%s", e->link_libs[j]);
        }
    }
}

/* ================================================================== */
/* CLI: tur new / tur init -- shared helpers                           */
/* ================================================================== */

/* Validate project name: must match [a-z][a-z0-9-]* */
static bool valid_project_name(const char *name) {
    if (!name || !*name) return false;
    if (!islower((unsigned char)*name)) return false;
    for (const char *p = name + 1; *p; p++) {
        if (!islower((unsigned char)*p) &&
            !isdigit((unsigned char)*p) &&
            *p != '-')
            return false;
    }
    return true;
}

/* Scaffold a new project inside 'dir' (must already exist).
 * 'name' is the project name string used in generated files.
 * Returns 0 on success. */
static int scaffold_project(const char *dir, const char *name,
                             bool is_bin, bool no_git) {
    char path[4096];

    /* Create src/ */
    snprintf(path, sizeof(path), "%s/src", dir);
    if (!mkdirp(path)) {
        fprintf(stderr, "tur: cannot create '%s'\n", path);
        return 1;
    }

    /* Write build.tur */
    snprintf(path, sizeof(path), "%s/build.tur", dir);
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    m.name    = (char *)name;
    m.version = "0.1.0";
    if (!pkg_manifest_write(path, &m)) return 1;

    /* Write tur.lock */
    snprintf(path, sizeof(path), "%s/tur.lock", dir);
    PkgLockFile lock;
    memset(&lock, 0, sizeof(lock));
    lock.format_version = 1;
    if (!pkg_lock_write(path, &lock)) return 1;

    /* Write src/main.tur or src/lib.tur */
    if (is_bin) {
        snprintf(path, sizeof(path), "%s/src/main.tur", dir);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "#lang turmeric\n\n");
            fprintf(f, "(defn main [] :int\n");
            fprintf(f, "  (println \"Hello from %s!\")\n", name);
            fprintf(f, "  0)\n");
            fclose(f);
        }
    } else {
        snprintf(path, sizeof(path), "%s/src/lib.tur", dir);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "#lang turmeric\n\n");
            fprintf(f, ";;; greet -- return a greeting string.\n");
            fprintf(f, ";;;\n");
            fprintf(f, ";;; Parameters:\n");
            fprintf(f, ";;;   name -- the name to greet\n");
            fprintf(f, ";;;\n");
            fprintf(f, ";;; Returns:\n");
            fprintf(f, ";;;   A greeting string.\n");
            fprintf(f, ";;;\n");
            fprintf(f, ";;; Example:\n");
            fprintf(f, ";;;   (greet \"world\")  ; => \"Hello, world!\"\n");
            fprintf(f, ";;;\n");
            fprintf(f, ";;; Since: 0.1.0\n");
            fprintf(f, "(defn greet [name :str] :str\n");
            fprintf(f, "  (str \"Hello, \" name \"!\"))\n");
            fclose(f);
        }
    }

    /* Write .gitignore */
    snprintf(path, sizeof(path), "%s/.gitignore", dir);
    FILE *gi = fopen(path, "w");
    if (gi) {
        fprintf(gi, "build/\nspices/\ncmake/CMakeLists.txt\ncmake/build/\ncmake/spice-deps-manifest.json\n");
        fclose(gi);
    }

    /* Write README.md */
    snprintf(path, sizeof(path), "%s/README.md", dir);
    FILE *rm = fopen(path, "w");
    if (rm) {
        fprintf(rm, "# %s\n\nA Turmeric %s project.\n",
                name, is_bin ? "binary" : "library");
        fclose(rm);
    }

    /* Git init + initial commit */
    if (!no_git) {
        char cmd[4096];
        snprintf(cmd, sizeof(cmd),
            "git -C '%s' init -q 2>/dev/null && "
            "git -C '%s' add -A 2>/dev/null && "
            "git -C '%s' commit -q -m 'Initial commit' 2>/dev/null",
            dir, dir, dir);
        if (system(cmd) != 0) { /* git absent or commit failed -- non-fatal */ }
    }

    printf("Created %s project '%s'\n", is_bin ? "binary" : "library", name);
    printf("  build.tur\n");
    printf("  tur.lock\n");
    printf("  src/%s\n", is_bin ? "main.tur" : "lib.tur");
    printf("  .gitignore\n");
    printf("  README.md\n");
    if (!no_git) printf("  .git/\n");
    printf("\nRun:\n");
    if (strcmp(dir, ".") != 0)
        printf("  cd %s && tur run\n", name);
    else
        printf("  tur run\n");
    return 0;
}

/* ================================================================== */
/* CLI: tur new                                                         */
/* ================================================================== */

int cmd_pkg_new(int argc, char **argv) {
    /* Usage: tur new <name> [--bin|--lib] [--no-git] */
    bool is_bin = true;
    bool no_git = false;
    const char *name = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--bin") == 0)         is_bin = true;
        else if (strcmp(argv[i], "--lib") == 0)    is_bin = false;
        else if (strcmp(argv[i], "--no-git") == 0) no_git = true;
        else if (argv[i][0] != '-') {
            if (name) {
                fprintf(stderr, "tur new: unexpected argument '%s'\n", argv[i]);
                return 1;
            }
            name = argv[i];
        }
    }

    if (!name) {
        fprintf(stderr, "usage: tur new <name> [--bin|--lib] [--no-git]\n");
        return 1;
    }

    if (!valid_project_name(name)) {
        fprintf(stderr,
            "tur new: invalid project name '%s'\n"
            "  Names must match [a-z][a-z0-9-]* "
            "(lowercase letters, digits, hyphens)\n",
            name);
        return 1;
    }

    /* Directory must not already exist */
    struct stat st;
    if (stat(name, &st) == 0) {
        fprintf(stderr, "tur new: '%s' already exists\n", name);
        return 1;
    }

    if (!mkdirp(name)) {
        fprintf(stderr, "tur new: cannot create directory '%s': %s\n",
                name, strerror(errno));
        return 1;
    }

    return scaffold_project(name, name, is_bin, no_git);
}

/* ================================================================== */
/* CLI: tur init                                                        */
/* ================================================================== */

int cmd_pkg_init(int argc, char **argv) {
    /* Usage: tur init [--bin|--lib] [--no-git] [<name>] */
    bool is_bin = true;
    bool no_git = false;
    const char *name = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--bin") == 0)         is_bin = true;
        else if (strcmp(argv[i], "--lib") == 0)    is_bin = false;
        else if (strcmp(argv[i], "--no-git") == 0) no_git = true;
        else if (argv[i][0] != '-') {
            if (name) {
                fprintf(stderr, "tur init: unexpected argument '%s'\n", argv[i]);
                return 1;
            }
            name = argv[i];
        }
    }

    /* Derive name from current directory basename if not provided */
    char pwd_name[256];
    if (!name) {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) {
            fprintf(stderr, "tur init: cannot get current directory\n");
            return 1;
        }
        const char *base = strrchr(cwd, '/');
        strncpy(pwd_name, base ? base + 1 : cwd, sizeof(pwd_name) - 1);
        pwd_name[sizeof(pwd_name) - 1] = '\0';
        name = pwd_name;
    }

    if (!valid_project_name(name)) {
        fprintf(stderr,
            "tur init: invalid project name '%s'\n"
            "  Names must match [a-z][a-z0-9-]* "
            "(lowercase letters, digits, hyphens)\n",
            name);
        return 1;
    }

    /* Refuse if build.tur already exists */
    struct stat st;
    if (stat("build.tur", &st) == 0) {
        fprintf(stderr, "tur init: build.tur already exists\n");
        return 1;
    }

    return scaffold_project(".", name, is_bin, no_git);
}

/* ================================================================== */
/* Form-based build.tur mutation (comment-preserving round-trip)        */
/* ================================================================== */

/* Intern a keyword symbol by C string name. */
static const Symbol *pkg_intern(SymbolTable *st, const char *name) {
    return symtab_intern(st, strslice(name, (uint32_t)strlen(name)));
}

/* Build a spice entry value map: #{ :url "..." :ref "..." }
 * or #{ :path "..." } for local deps. */
static Form *pkg_build_spice_val(Arena *a, SymbolTable *st,
                                  const char *url, const char *ref,
                                  const char *path, bool optional) {
    Form *items[8];
    uint32_t n = 0;
    if (path) {
        items[n++] = form_keyword(a, SPAN_UNKNOWN, pkg_intern(st, "path"));
        items[n++] = form_str(a, SPAN_UNKNOWN, path, (uint32_t)strlen(path));
    } else {
        items[n++] = form_keyword(a, SPAN_UNKNOWN, pkg_intern(st, "url"));
        items[n++] = form_str(a, SPAN_UNKNOWN, url ? url : "",
                              (uint32_t)strlen(url ? url : ""));
        if (ref) {
            items[n++] = form_keyword(a, SPAN_UNKNOWN, pkg_intern(st, "ref"));
            items[n++] = form_str(a, SPAN_UNKNOWN, ref, (uint32_t)strlen(ref));
        }
    }
    if (optional) {
        items[n++] = form_keyword(a, SPAN_UNKNOWN, pkg_intern(st, "optional"));
        items[n++] = form_bool(a, SPAN_UNKNOWN, true);
    }
    return form_map(a, SPAN_UNKNOWN, items, n);
}

/* Return a new defpackage Form (F_LIST) with a new spice entry appended.
 * If :spices already exists in dp, the new key/val are appended to the map.
 * If :spices is absent, a new :spices #{} map is added. */
static Form *pkg_defpackage_add_spice(Arena *a, SymbolTable *st,
                                       const Form *dp,
                                       const char *spice_name,
                                       const char *url, const char *ref,
                                       const char *path, bool optional) {
    Form *entry_key = form_str(a, SPAN_UNKNOWN,
                               spice_name, (uint32_t)strlen(spice_name));
    Form *entry_val = pkg_build_spice_val(a, st, url, ref, path, optional);

    uint32_t n = dp->as.list.len;

    /* Find :spices keyword index */
    int spices_val_idx = -1;
    for (uint32_t i = 0; i + 1 < n; i++) {
        const Form *kf = dp->as.list.items[i];
        if (kf->tag == F_KEYWORD &&
            strcmp(kf->as.sym->name, "spices") == 0) {
            spices_val_idx = (int)(i + 1);
            break;
        }
    }

    /* Preserve the original form's span so comment gap extraction works. */
    Span orig_span = dp->span;

    if (spices_val_idx >= 0) {
        /* Extend existing :spices map */
        const Form *old_map = dp->as.list.items[spices_val_idx];
        uint32_t map_n = (old_map->tag == F_MAP) ? old_map->as.list.len : 0;
        Form **new_map_items = (Form **)arena_alloc(
                a, (map_n + 2) * sizeof(Form *));
        if (map_n > 0)
            memcpy(new_map_items, old_map->as.list.items,
                   map_n * sizeof(Form *));
        new_map_items[map_n]     = entry_key;
        new_map_items[map_n + 1] = entry_val;
        Form *new_map = form_map(a, SPAN_UNKNOWN, new_map_items, map_n + 2);

        Form **new_dp = (Form **)arena_alloc(a, n * sizeof(Form *));
        memcpy(new_dp, dp->as.list.items, n * sizeof(Form *));
        new_dp[spices_val_idx] = new_map;
        return form_list(a, orig_span, new_dp, n);
    } else {
        /* Append :spices #{ entry } */
        Form *kw = form_keyword(a, SPAN_UNKNOWN, pkg_intern(st, "spices"));
        Form *new_map_items[2] = { entry_key, entry_val };
        Form *new_map = form_map(a, SPAN_UNKNOWN, new_map_items, 2);

        Form **new_dp = (Form **)arena_alloc(a, (n + 2) * sizeof(Form *));
        memcpy(new_dp, dp->as.list.items, n * sizeof(Form *));
        new_dp[n]     = kw;
        new_dp[n + 1] = new_map;
        return form_list(a, orig_span, new_dp, n + 2);
    }
}

/* Read build.tur, add a spice entry, and write back preserving comments.
 * Returns true on success. */
static bool pkg_build_tur_add_spice(const char *build_path,
                                     const char *spice_name,
                                     const char *url, const char *ref,
                                     const char *path, bool optional) {
    /* 1. Read raw source (keep for comment preservation). */
    FILE *f = fopen(build_path, "rb");
    if (!f) {
        fprintf(stderr, "tur add: cannot open '%s': %s\n",
                build_path, strerror(errno));
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char *)malloc((size_t)sz + 1);
    if (!src) { fclose(f); return false; }
    if (fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(src); return false;
    }
    fclose(f);
    src[sz] = '\0';

    /* 2. Parse into Form AST. */
    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    SourceFile file = {0};
    file.path    = build_path;
    file.src     = src;
    file.len     = (uint32_t)sz;
    file.file_id = 0;
    diag_register_file(&file);
    diag_reset();

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    if (!forms || diag_had_error() || nforms == 0) {
        fprintf(stderr, "tur add: failed to parse '%s'\n", build_path);
        free(src);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    /* 3. Find the defpackage form and mutate it. */
    int dp_idx = -1;
    for (uint32_t i = 0; i < nforms; i++) {
        const Form *frm = forms[i];
        if (frm->tag == F_LIST && frm->as.list.len >= 2) {
            const Form *head = frm->as.list.items[0];
            if (head->tag == F_SYM &&
                strcmp(head->as.sym->name, "defpackage") == 0) {
                dp_idx = (int)i;
                break;
            }
        }
    }
    if (dp_idx < 0) {
        fprintf(stderr, "tur add: no (defpackage ...) form in '%s'\n",
                build_path);
        free(src);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    forms[dp_idx] = pkg_defpackage_add_spice(
            &arena, &st, forms[dp_idx],
            spice_name, url, ref, path, optional);

    /* 4. Re-emit with comment preservation. */
    Buf out;
    buf_init(&out);
    FmtOptions opts = {0};
    opts.indent_width = 2;
    opts.line_width   = 80;
    opts.src          = src;
    opts.src_len      = (size_t)sz;
    if (fmt_print(&out, forms, nforms, opts) != 0) {
        fprintf(stderr, "tur add: formatter failed\n");
        buf_free(&out);
        free(src);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    /* 5. Write atomically (temp file + rename). */
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", build_path, (int)getpid());
    FILE *wf = fopen(tmp_path, "wb");
    if (!wf) {
        fprintf(stderr, "tur add: cannot write '%s': %s\n",
                tmp_path, strerror(errno));
        buf_free(&out);
        free(src);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }
    fwrite(out.data, 1, out.len, wf);
    fclose(wf);

    if (rename(tmp_path, build_path) != 0) {
        fprintf(stderr, "tur add: rename failed: %s\n", strerror(errno));
        unlink(tmp_path);
        buf_free(&out);
        free(src);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    buf_free(&out);
    free(src);
    symtab_free(&st);
    arena_free(&arena);
    return true;
}

/* ================================================================== */
/* CLI: tur add                                                         */
/* ================================================================== */

int cmd_pkg_add(int argc, char **argv) {
    /* Usage: tur add <url-or-path> [--ref <ref>] [--name <name>]
     *                              [--path] [--optional] */
    const char *url_or_path   = NULL;
    const char *ref           = NULL;
    const char *name_override = NULL;
    bool        is_path       = false;
    bool        optional      = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ref") == 0 && i + 1 < argc) {
            ref = argv[++i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name_override = argv[++i];
        } else if (strcmp(argv[i], "--path") == 0) {
            is_path = true;
        } else if (strcmp(argv[i], "--optional") == 0) {
            optional = true;
        } else if (argv[i][0] != '-') {
            if (url_or_path) {
                fprintf(stderr, "tur add: unexpected argument '%s'\n", argv[i]);
                return 1;
            }
            url_or_path = argv[i];
        }
    }
    if (!url_or_path) {
        fprintf(stderr, "usage: tur add <url> [--ref <ref>] [--name <name>]\n"
                        "       tur add <path> --path\n");
        return 1;
    }

    /* Handle spice/<pkg> registry shorthand */
    if (strncmp(url_or_path, "spice/", 6) == 0) {
        fprintf(stderr,
            "The Spice registry is not yet available.\n"
            "Add the package directly with a Git URL:\n"
            "  tur add https://github.com/turmeric-spice/tur-%s --ref v0.1.0\n",
            url_or_path + 6);
        return 1;
    }

    /* Load existing build.tur */
    struct stat st;
    if (stat("build.tur", &st) != 0) {
        fprintf(stderr,
            "No build.tur found. Run `tur new <name>` to create a project.\n");
        return 1;
    }
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read("build.tur", &m)) return 1;

    /* Derive package name from URL/path or --name override */
    char name_buf[256];
    if (name_override) {
        strncpy(name_buf, name_override, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
    } else {
        const char *last = strrchr(url_or_path, '/');
        const char *base = last ? last + 1 : url_or_path;
        strncpy(name_buf, base, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        /* strip .git suffix */
        size_t nl = strlen(name_buf);
        if (nl > 4 && strcmp(name_buf + nl - 4, ".git") == 0)
            name_buf[nl - 4] = '\0';
        /* strip tur- prefix for nicer names */
        if (strncmp(name_buf, "tur-", 4) == 0)
            memmove(name_buf, name_buf + 4, strlen(name_buf) - 3);
    }

    /* Check for duplicate */
    for (int i = 0; i < m.n_spices; i++) {
        if (strcmp(m.spices[i].name, name_buf) == 0) {
            fprintf(stderr,
                "'%s' is already a dependency. "
                "Use `tur update %s` to change the ref.\n",
                name_buf, name_buf);
            pkg_manifest_free(&m);
            return 1;
        }
    }

    /* Warn when no --ref given for a git dep */
    if (!is_path && !ref) {
        fprintf(stderr,
            "Warning: no --ref specified; will resolve to HEAD.\n"
            "Pin with: tur add %s --ref <tag-or-sha>\n",
            url_or_path);
    }

    /* Write build.tur via form-based round-trip (preserves comments). */
    const char *spice_url  = is_path ? NULL : url_or_path;
    const char *spice_path = is_path ? url_or_path : NULL;
    if (!pkg_build_tur_add_spice("build.tur", name_buf,
                                  spice_url, ref, spice_path, optional)) {
        pkg_manifest_free(&m);
        return 1;
    }

    /* Also track in the manifest struct for the fetch step below. */
    m.spices = (PkgSpice *)realloc(m.spices,
                                    (m.n_spices + 1) * sizeof(PkgSpice));
    if (!m.spices) { pkg_manifest_free(&m); return 1; }
    PkgSpice *ns = &m.spices[m.n_spices++];
    memset(ns, 0, sizeof(*ns));
    ns->name     = tur_strdup(name_buf);
    ns->optional = optional;
    if (is_path) {
        ns->path = tur_strdup(url_or_path);
    } else {
        ns->url = tur_strdup(url_or_path);
        if (ref) ns->ref = tur_strdup(ref);
    }

    /* Fetch immediately and update tur.lock */
    if (!is_path) {
        PkgLockFile lock;
        memset(&lock, 0, sizeof(lock));
        lock.format_version = 1;
        pkg_lock_read("tur.lock", &lock);

        bool fetch_ok = pkg_fetch_all(".", &m, &lock, false);
        if (fetch_ok) {
            pkg_lock_write("tur.lock", &lock);
            /* If no ref was given, report what HEAD resolved to */
            if (!ref) {
                PkgLockEntry *le = pkg_lock_find(&lock, name_buf, false);
                if (le && le->resolved) {
                    fprintf(stderr,
                        "Warning: no --ref specified; resolved to HEAD (%s).\n"
                        "Pin with: tur add %s --ref %.12s\n",
                        le->resolved, url_or_path, le->resolved);
                }
            }
        } else {
            fprintf(stderr,
                "tur add: fetch failed -- build.tur was updated but "
                "tur.lock was not. Run `tur fetch` to retry.\n");
        }
        pkg_lock_free(&lock);
    }

    printf("Added '%s'", name_buf);
    if (ns->url) printf(" -> %s", ns->url);
    if (ns->ref) printf(" @ %s", ns->ref);
    if (ns->path) printf(" (local: %s)", ns->path);
    printf("\n");
    if (!is_path) {
        printf("  build.tur updated\n");
        printf("  tur.lock updated\n");
    }

    pkg_manifest_free(&m);
    return 0;
}

/* ================================================================== */
/* CLI: tur add-cmake                                                   */
/* ================================================================== */

int cmd_pkg_add_cmake(int argc, char **argv) {
    /* Usage: tur add-cmake <url> [--ref <ref>] [--opt KEY=VAL ...] */
    const char *url = NULL;
    const char *ref = NULL;
    /* Options as "KEY=VALUE" strings (max 16) */
    char *opts[16];
    int n_opts = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ref") == 0 && i + 1 < argc) {
            ref = argv[++i];
        } else if (strcmp(argv[i], "--opt") == 0 && i + 1 < argc) {
            if (n_opts < 16) opts[n_opts++] = argv[++i];
        } else if (argv[i][0] != '-') {
            if (url) {
                fprintf(stderr, "tur add-cmake: unexpected '%s'\n", argv[i]);
                return 1;
            }
            url = argv[i];
        }
    }
    if (!url) {
        fprintf(stderr, "usage: tur add-cmake <url> [--ref <ref>] "
                        "[--opt KEY=VAL]\n");
        return 1;
    }

    struct stat st;
    if (stat("build.tur", &st) != 0) {
        fprintf(stderr, "tur add-cmake: no build.tur; run `tur init` first\n");
        return 1;
    }

    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read("build.tur", &m)) return 1;

    /* Derive name from URL */
    char name_buf[256];
    {
        const char *last = strrchr(url, '/');
        const char *base = last ? last + 1 : url;
        strncpy(name_buf, base, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        size_t nl = strlen(name_buf);
        if (nl > 4 && strcmp(name_buf + nl - 4, ".git") == 0)
            name_buf[nl - 4] = '\0';
    }

    /* Check for duplicate */
    for (int i = 0; i < m.n_cmake_deps; i++) {
        if (strcmp(m.cmake_deps[i].name, name_buf) == 0) {
            fprintf(stderr, "tur add-cmake: '%s' already in build.tur\n",
                    name_buf);
            pkg_manifest_free(&m);
            return 1;
        }
    }

    m.cmake_deps = (PkgCmakeDep *)realloc(m.cmake_deps,
        (m.n_cmake_deps + 1) * sizeof(PkgCmakeDep));
    if (!m.cmake_deps) { pkg_manifest_free(&m); return 1; }
    PkgCmakeDep *nd = &m.cmake_deps[m.n_cmake_deps++];
    memset(nd, 0, sizeof(*nd));
    nd->name = tur_strdup(name_buf);
    nd->url  = tur_strdup(url);
    if (ref) nd->ref = tur_strdup(ref);

    if (n_opts > 0) {
        nd->opts   = (PkgCmakeOpt *)malloc(n_opts * sizeof(PkgCmakeOpt));
        nd->n_opts = 0;
        for (int i = 0; i < n_opts; i++) {
            char *eq = strchr(opts[i], '=');
            if (!eq) continue;
            *eq = '\0';
            nd->opts[nd->n_opts].key = tur_strdup(opts[i]);
            nd->opts[nd->n_opts].val = tur_strdup(eq + 1);
            nd->n_opts++;
            *eq = '=';
        }
    }

    if (!pkg_manifest_write("build.tur", &m)) {
        pkg_manifest_free(&m);
        return 1;
    }

    printf("Added cmake dep '%s' -> %s", name_buf, url);
    if (ref) printf(" @ %s", ref);
    printf("\n");
    printf("Run `tur fetch` to generate cmake/SpiceDeps.cmake.\n");

    pkg_manifest_free(&m);
    return 0;
}

/* ================================================================== */
/* CLI: tur fetch                                                       */
/* ================================================================== */

int cmd_pkg_fetch(int argc, char **argv) {
    bool update = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--update") == 0) update = true;
    }

    struct stat st;
    if (stat("build.tur", &st) != 0) {
        fprintf(stderr, "tur fetch: no build.tur found in current directory\n");
        return 1;
    }

    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read("build.tur", &m)) return 1;

    PkgLockFile lock;
    memset(&lock, 0, sizeof(lock));
    lock.format_version = 1;
    /* Load existing lock (if any) */
    pkg_lock_read("tur.lock", &lock);

    bool ok = pkg_fetch_all(".", &m, &lock, update);

    /* cmake deps: generate cmake/CMakeLists.txt, then configure+build */
    if (m.n_cmake_deps > 0) {
        /* On re-fetch without --update, verify existing SHAs first */
        if (!update) {
            if (!pkg_cmake_verify_lock(".", &lock)) {
                fprintf(stderr,
                    "spice: SHA mismatch detected -- run `tur fetch --update`"
                    " to re-fetch cmake deps\n");
                ok = false;
            }
        }
        if (ok) {
            if (!pkg_gen_cmake_deps(".", &m)) {
                ok = false;
            } else if (!pkg_cmake_build(".", &m, &lock)) {
                ok = false;
            }
        }
    }

    /* Write updated lock file */
    if (!pkg_lock_write("tur.lock", &lock)) ok = false;

    pkg_lock_free(&lock);
    pkg_manifest_free(&m);

    if (ok) {
        printf("spice: lock file written to tur.lock\n");
    } else {
        fprintf(stderr, "spice: fetch completed with errors\n");
        return 1;
    }
    return 0;
}

/* ================================================================== */
/* CLI: tur emit-cmake (Phase B)                                       */
/* ================================================================== */

/* FindTurmeric.cmake -- distributed with tur, placed in cmake/ by emit-cmake */
static const char FIND_TURMERIC_CMAKE[] =
"# FindTurmeric.cmake -- distributed with the Turmeric toolchain.\n"
"#\n"
"# Imported targets:\n"
"#   Turmeric::Compiler -- the tur binary as an imported executable\n"
"#\n"
"# Variables set:\n"
"#   Turmeric_FOUND, Turmeric_VERSION, Turmeric_COMPILER\n"
"\n"
"find_program(Turmeric_COMPILER NAMES tur\n"
"  PATHS ENV PATH /usr/local/bin /opt/homebrew/bin\n"
"  DOC \"Path to the Turmeric compiler\"\n"
")\n"
"\n"
"if(Turmeric_COMPILER)\n"
"  execute_process(\n"
"    COMMAND \"${Turmeric_COMPILER}\" --version\n"
"    OUTPUT_VARIABLE Turmeric_VERSION_STRING\n"
"    OUTPUT_STRIP_TRAILING_WHITESPACE\n"
"    ERROR_QUIET\n"
"  )\n"
"  string(REGEX MATCH \"[0-9]+\\\\.[0-9]+\\\\.[0-9]+\" Turmeric_VERSION\n"
"         \"${Turmeric_VERSION_STRING}\")\n"
"endif()\n"
"\n"
"include(FindPackageHandleStandardArgs)\n"
"find_package_handle_standard_args(Turmeric\n"
"  REQUIRED_VARS Turmeric_COMPILER\n"
"  VERSION_VAR   Turmeric_VERSION\n"
")\n"
"\n"
"if(Turmeric_FOUND AND NOT TARGET Turmeric::Compiler)\n"
"  add_executable(Turmeric::Compiler IMPORTED)\n"
"  set_target_properties(Turmeric::Compiler PROPERTIES\n"
"    IMPORTED_LOCATION \"${Turmeric_COMPILER}\"\n"
"  )\n"
"endif()\n";

/* AddTurmericTarget.cmake -- distributed with tur, placed in cmake/ by emit-cmake */
static const char ADD_TURMERIC_TARGET_CMAKE[] =
"# AddTurmericTarget.cmake -- helper functions for CMake projects using Turmeric.\n"
"#\n"
"# Functions:\n"
"#   add_turmeric_library(<name> SOURCES <...> [DEPENDS <targets...>])\n"
"#   add_turmeric_executable(<name> SOURCES <...> [LIBRARIES <targets...>])\n"
"\n"
"include_guard(GLOBAL)\n"
"find_package(Turmeric REQUIRED)\n"
"\n"
"# add_turmeric_library(<name> SOURCES <files...> [DEPENDS <targets...>])\n"
"function(add_turmeric_library name)\n"
"  cmake_parse_arguments(ATL \"\" \"\" \"SOURCES;DEPENDS\" ${ARGN})\n"
"  if(NOT ATL_SOURCES)\n"
"    message(FATAL_ERROR \"add_turmeric_library: SOURCES is required\")\n"
"  endif()\n"
"\n"
"  # Derive the list of expected .h and .c outputs\n"
"  set(_out_h_files)\n"
"  set(_out_c_files)\n"
"  foreach(_src IN LISTS ATL_SOURCES)\n"
"    get_filename_component(_mod \"${_src}\" NAME_WE)\n"
"    list(APPEND _out_h_files \"${CMAKE_CURRENT_BINARY_DIR}/${_mod}.h\")\n"
"    list(APPEND _out_c_files \"${CMAKE_CURRENT_BINARY_DIR}/${_mod}.c\")\n"
"  endforeach()\n"
"\n"
"  add_custom_command(\n"
"    OUTPUT  ${_out_h_files} ${_out_c_files}\n"
"    COMMAND Turmeric::Compiler emit-c\n"
"            --output-dir \"${CMAKE_CURRENT_BINARY_DIR}\"\n"
"            ${ATL_SOURCES}\n"
"    DEPENDS ${ATL_SOURCES}\n"
"    COMMENT \"Compiling Turmeric library ${name}\"\n"
"  )\n"
"\n"
"  add_library(${name} STATIC ${_out_c_files})\n"
"  target_include_directories(${name} PUBLIC \"${CMAKE_CURRENT_BINARY_DIR}\")\n"
"\n"
"  if(ATL_DEPENDS)\n"
"    target_link_libraries(${name} PUBLIC ${ATL_DEPENDS})\n"
"  endif()\n"
"\n"
"  add_library(${name}::${name} ALIAS ${name})\n"
"endfunction()\n"
"\n"
"# add_turmeric_executable(<name> SOURCES <files...> [LIBRARIES <targets...>])\n"
"function(add_turmeric_executable name)\n"
"  cmake_parse_arguments(ATE \"\" \"\" \"SOURCES;LIBRARIES\" ${ARGN})\n"
"  if(NOT ATE_SOURCES)\n"
"    message(FATAL_ERROR \"add_turmeric_executable: SOURCES is required\")\n"
"  endif()\n"
"\n"
"  set(_out_h_files)\n"
"  set(_out_c_files)\n"
"  foreach(_src IN LISTS ATE_SOURCES)\n"
"    get_filename_component(_mod \"${_src}\" NAME_WE)\n"
"    list(APPEND _out_h_files \"${CMAKE_CURRENT_BINARY_DIR}/${_mod}.h\")\n"
"    list(APPEND _out_c_files \"${CMAKE_CURRENT_BINARY_DIR}/${_mod}.c\")\n"
"  endforeach()\n"
"\n"
"  add_custom_command(\n"
"    OUTPUT  ${_out_h_files} ${_out_c_files}\n"
"    COMMAND Turmeric::Compiler emit-c\n"
"            --output-dir \"${CMAKE_CURRENT_BINARY_DIR}\"\n"
"            ${ATE_SOURCES}\n"
"    DEPENDS ${ATE_SOURCES}\n"
"    COMMENT \"Compiling Turmeric executable ${name}\"\n"
"  )\n"
"\n"
"  add_executable(${name} ${_out_c_files})\n"
"\n"
"  if(ATE_LIBRARIES)\n"
"    target_link_libraries(${name} PRIVATE ${ATE_LIBRARIES})\n"
"  endif()\n"
"endfunction()\n";

/* Collect .tur files from src/ when :exports is absent.
 * Returns a malloc'd array of heap-alloc'd paths; *n_out = count.
 * Caller must free each string and the array. */
static char **collect_exports_from_src(const char *project_dir, int *n_out) {
    *n_out = 0;
    char src_dir[4096];
    snprintf(src_dir, sizeof(src_dir), "%s/src", project_dir);

    DIR *d = opendir(src_dir);
    if (!d) return NULL;

    int cap = 8;
    char **files = (char **)malloc(cap * sizeof(char *));
    if (!files) { closedir(d); return NULL; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
#if defined(_DIRENT_HAVE_D_TYPE) || defined(__APPLE__) || defined(__FreeBSD__)
        if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
#endif
        size_t len = strlen(ent->d_name);
        if (len < 4 || strcmp(ent->d_name + len - 4, ".tur") != 0) continue;

        if (*n_out >= cap) {
            cap *= 2;
            files = (char **)realloc(files, cap * sizeof(char *));
            if (!files) { closedir(d); return NULL; }
        }
        char full[4096];
        snprintf(full, sizeof(full), "src/%s", ent->d_name);
        files[(*n_out)++] = tur_strdup(full);
    }
    closedir(d);
    return files;
}

/* Write a text file atomically.  Returns true on success. */
static bool write_file_atomic(const char *path, const char *content) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "tur emit-cmake: cannot write '%s': %s\n",
                path, strerror(errno));
        return false;
    }
    fputs(content, f);
    fclose(f);
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "tur emit-cmake: rename failed: %s\n", strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

int cmd_pkg_emit_cmake(int argc, char **argv) {
    /* Usage: tur emit-cmake [--output-dir <dir>]
     * Reads build.tur in the current directory and generates:
     *   CMakeLists.txt
     *   TurmericConfig.cmake
     *   cmake/FindTurmeric.cmake
     *   cmake/AddTurmericTarget.cmake */
    const char *output_dir = ".";

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "tur emit-cmake: unknown flag '%s'\n", argv[i]);
            return 1;
        }
    }

    struct stat st;
    if (stat("build.tur", &st) != 0) {
        fprintf(stderr,
            "tur emit-cmake: no build.tur found in current directory\n"
            "  Run `tur init --lib <name>` to create a library project.\n");
        return 1;
    }

    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read("build.tur", &m)) return 1;

    if (!m.name) {
        fprintf(stderr, "tur emit-cmake: build.tur has no :name field\n");
        pkg_manifest_free(&m);
        return 1;
    }

    const char *version = m.version ? m.version : "0.0.0";

    /* Collect source files from :exports or scan src/ */
    char **sources = NULL;
    int    n_sources = 0;
    bool   sources_heap = false;

    if (m.n_exports > 0) {
        sources   = m.exports;
        n_sources = m.n_exports;
    } else {
        sources       = collect_exports_from_src(".", &n_sources);
        sources_heap  = true;
        if (!sources || n_sources == 0) {
            fprintf(stderr,
                "tur emit-cmake: no source files found.\n"
                "  Add a :exports [\"src/lib.tur\"] block to build.tur or\n"
                "  create .tur files in src/.\n");
            pkg_manifest_free(&m);
            return 1;
        }
    }

    /* Ensure cmake/ subdirectory exists */
    char cmake_subdir[4096];
    snprintf(cmake_subdir, sizeof(cmake_subdir), "%s/cmake", output_dir);
    if (!mkdirp(cmake_subdir)) {
        fprintf(stderr, "tur emit-cmake: cannot create '%s'\n", cmake_subdir);
        if (sources_heap) {
            for (int i = 0; i < n_sources; i++) free(sources[i]);
            free(sources);
        }
        pkg_manifest_free(&m);
        return 1;
    }

    /* Build the CMakeLists.txt content */
    Buf cml;
    buf_init(&cml);
    buf_printf(&cml,
        "# CMakeLists.txt -- AUTO-GENERATED by `tur emit-cmake`. Do not edit.\n"
        "# Re-generate with: tur emit-cmake\n"
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(%s VERSION %s LANGUAGES C)\n"
        "\n"
        "list(APPEND CMAKE_MODULE_PATH \"${CMAKE_CURRENT_SOURCE_DIR}/cmake\")\n"
        "find_package(Turmeric REQUIRED)\n"
        "\n",
        m.name, version);

    /* Collect output file lists for the custom command */
    Buf out_files, dep_files, src_args;
    buf_init(&out_files);
    buf_init(&dep_files);
    buf_init(&src_args);

    for (int i = 0; i < n_sources; i++) {
        /* Extract module name (basename without .tur) */
        const char *src = sources[i];
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        size_t blen = strlen(base);
        char mod[256];
        size_t mlen = (blen >= 4 && strcmp(base + blen - 4, ".tur") == 0)
                      ? blen - 4 : blen;
        if (mlen >= sizeof(mod)) mlen = sizeof(mod) - 1;
        memcpy(mod, base, mlen);
        mod[mlen] = '\0';

        buf_printf(&out_files,
            "          \"${CMAKE_CURRENT_BINARY_DIR}/%s.h\"\n"
            "          \"${CMAKE_CURRENT_BINARY_DIR}/%s.c\"\n",
            mod, mod);

        buf_printf(&dep_files,
            "          \"${CMAKE_CURRENT_SOURCE_DIR}/%s\"\n", src);

        buf_printf(&src_args,
            "              \"${CMAKE_CURRENT_SOURCE_DIR}/%s\"\n", src);
    }

    /* add_custom_command block */
    buf_printf(&cml,
        "add_custom_command(\n"
        "  OUTPUT\n"
        "%s"
        "  COMMAND Turmeric::Compiler emit-c\n"
        "              --output-dir \"${CMAKE_CURRENT_BINARY_DIR}\"\n"
        "%s"
        "  DEPENDS\n"
        "%s"
        "  COMMENT \"Compiling Turmeric library %s\"\n"
        ")\n\n",
        out_files.data, src_args.data, dep_files.data, m.name);

    buf_free(&out_files);
    buf_free(&dep_files);

    /* add_library: list all .c files */
    buf_printf(&cml, "add_library(%s STATIC\n", m.name);
    for (int i = 0; i < n_sources; i++) {
        const char *src = sources[i];
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        size_t blen = strlen(base);
        char mod[256];
        size_t mlen = (blen >= 4 && strcmp(base + blen - 4, ".tur") == 0)
                      ? blen - 4 : blen;
        if (mlen >= sizeof(mod)) mlen = sizeof(mod) - 1;
        memcpy(mod, base, mlen);
        mod[mlen] = '\0';
        buf_printf(&cml,
            "  \"${CMAKE_CURRENT_BINARY_DIR}/%s.c\"\n", mod);
    }
    buf_free(&src_args);

    buf_printf(&cml,
        ")\n"
        "target_include_directories(%s\n"
        "  PUBLIC\n"
        "    \"$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>\"\n"
        "    \"$<INSTALL_INTERFACE:include>\"\n"
        ")\n"
        "add_library(%s::all ALIAS %s)\n"
        "\n"
        "# Install rules\n"
        "install(TARGETS %s\n"
        "  EXPORT %s-targets\n"
        "  ARCHIVE DESTINATION lib\n"
        ")\n",
        m.name, m.name, m.name, m.name, m.name);

    /* install headers */
    for (int i = 0; i < n_sources; i++) {
        const char *src = sources[i];
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        size_t blen = strlen(base);
        char mod[256];
        size_t mlen = (blen >= 4 && strcmp(base + blen - 4, ".tur") == 0)
                      ? blen - 4 : blen;
        if (mlen >= sizeof(mod)) mlen = sizeof(mod) - 1;
        memcpy(mod, base, mlen);
        mod[mlen] = '\0';
        buf_printf(&cml,
            "install(FILES \"${CMAKE_CURRENT_BINARY_DIR}/%s.h\"\n"
            "  DESTINATION include\n"
            ")\n", mod);
    }

    buf_printf(&cml,
        "install(EXPORT %s-targets\n"
        "  FILE %s-targets.cmake\n"
        "  NAMESPACE %s::\n"
        "  DESTINATION lib/cmake/%s\n"
        ")\n"
        "install(FILES cmake/FindTurmeric.cmake cmake/AddTurmericTarget.cmake\n"
        "  DESTINATION lib/cmake/%s\n"
        ")\n",
        m.name, m.name, m.name, m.name, m.name);

    /* Write CMakeLists.txt */
    char cml_path[4096];
    snprintf(cml_path, sizeof(cml_path), "%s/CMakeLists.txt", output_dir);
    buf_putc(&cml, '\0');
    if (!write_file_atomic(cml_path, cml.data)) {
        buf_free(&cml);
        if (sources_heap) {
            for (int i = 0; i < n_sources; i++) free(sources[i]);
            free(sources);
        }
        pkg_manifest_free(&m);
        return 1;
    }
    buf_free(&cml);
    printf("  %s\n", cml_path);

    /* Build TurmericConfig.cmake content */
    Buf cfg;
    buf_init(&cfg);
    buf_printf(&cfg,
        "# %sConfig.cmake -- AUTO-GENERATED by `tur emit-cmake`. Do not edit.\n"
        "set(%s_VERSION %s)\n"
        "set(%s_FOUND   TRUE)\n"
        "include(\"${CMAKE_CURRENT_LIST_DIR}/%s-targets.cmake\")\n",
        m.name, m.name, version, m.name, m.name);
    buf_putc(&cfg, '\0');

    char cfg_path[4096];
    snprintf(cfg_path, sizeof(cfg_path), "%s/%sConfig.cmake", output_dir, m.name);
    if (!write_file_atomic(cfg_path, cfg.data)) {
        buf_free(&cfg);
        if (sources_heap) {
            for (int i = 0; i < n_sources; i++) free(sources[i]);
            free(sources);
        }
        pkg_manifest_free(&m);
        return 1;
    }
    buf_free(&cfg);
    printf("  %s\n", cfg_path);

    /* Write cmake/FindTurmeric.cmake */
    char find_path[4096];
    snprintf(find_path, sizeof(find_path), "%s/FindTurmeric.cmake", cmake_subdir);
    if (!write_file_atomic(find_path, FIND_TURMERIC_CMAKE)) {
        if (sources_heap) {
            for (int i = 0; i < n_sources; i++) free(sources[i]);
            free(sources);
        }
        pkg_manifest_free(&m);
        return 1;
    }
    printf("  %s\n", find_path);

    /* Write cmake/AddTurmericTarget.cmake */
    char add_path[4096];
    snprintf(add_path, sizeof(add_path),
             "%s/AddTurmericTarget.cmake", cmake_subdir);
    if (!write_file_atomic(add_path, ADD_TURMERIC_TARGET_CMAKE)) {
        if (sources_heap) {
            for (int i = 0; i < n_sources; i++) free(sources[i]);
            free(sources);
        }
        pkg_manifest_free(&m);
        return 1;
    }
    printf("  %s\n", add_path);

    /* Save copies before freeing manifest */
    char *pkg_name    = tur_strdup(m.name);
    char *pkg_version = tur_strdup(version);

    if (sources_heap) {
        for (int i = 0; i < n_sources; i++) free(sources[i]);
        free(sources);
    }
    pkg_manifest_free(&m);

    printf("\nGenerated cmake files for '%s' v%s.\n", pkg_name, pkg_version);
    printf("\nTo use from a CMake project via CPM:\n");
    printf("  CPMAddPackage(\n");
    printf("    NAME    %s\n", pkg_name);
    printf("    URL     https://github.com/<user>/%s/archive/refs/tags/v%s.tar.gz\n",
           pkg_name, pkg_version);
    printf("    VERSION %s\n", pkg_version);
    printf("  )\n");
    printf("  target_link_libraries(my_target PRIVATE %s::all)\n", pkg_name);

    free(pkg_name);
    free(pkg_version);
    return 0;
}
