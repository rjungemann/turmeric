/*
 * global.c -- per-user (global) spice install support.
 *
 * See global.h for the public API.
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
#include "forms.h"
#include "global.h"
#include "reader.h"
#include "symbols.h"
#include "platform_fs.h"

/* ------------------------------------------------------------------ */
/* Local helpers                                                       */
/* ------------------------------------------------------------------ */

static char *gs_ss_dup(StrSlice s) {
    char *out = (char *)malloc(s.len + 1);
    if (!out) return NULL;
    memcpy(out, s.p, s.len);
    out[s.len] = '\0';
    return out;
}

__attribute__((unused))
static const char *gs_iso_now(void) {
    static char buf[32];
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

/* Recursive mkdir. */
static bool gs_mkdirp(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    if (mkdir(path, 0755) == 0) return true;
    if (errno == EEXIST) return true;

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

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */

bool tur_global_data_dir(char *out, size_t out_sz) {
    const char *th = getenv("TUR_HOME");
    if (th && th[0]) {
        snprintf(out, out_sz, "%s", th);
        return true;
    }
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, out_sz, "%s/turmeric", xdg);
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        fprintf(stderr, "tur: $HOME is not set; cannot locate data dir\n");
        return false;
    }
    snprintf(out, out_sz, "%s/.local/share/turmeric", home);
    return true;
}

bool tur_global_bin_dir(char *out, size_t out_sz) {
    const char *th = getenv("TUR_HOME");
    if (th && th[0]) {
        snprintf(out, out_sz, "%s/bin", th);
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        fprintf(stderr, "tur: $HOME is not set; cannot locate bin dir\n");
        return false;
    }
    snprintf(out, out_sz, "%s/.local/bin", home);
    return true;
}

bool tur_global_spices_dir(char *out, size_t out_sz) {
    char data[4096];
    if (!tur_global_data_dir(data, sizeof(data))) return false;
    snprintf(out, out_sz, "%s/spices", data);
    return true;
}

bool tur_global_cache_dir(char *out, size_t out_sz) {
    char data[4096];
    if (!tur_global_data_dir(data, sizeof(data))) return false;
    snprintf(out, out_sz, "%s/cache", data);
    return true;
}

bool tur_global_state_path(char *out, size_t out_sz) {
    char data[4096];
    if (!tur_global_data_dir(data, sizeof(data))) return false;
    snprintf(out, out_sz, "%s/state.tur", data);
    return true;
}

bool tur_global_ensure_layout(void) {
    char p[4096];
    if (!tur_global_data_dir(p, sizeof(p)) || !gs_mkdirp(p)) return false;
    if (!tur_global_spices_dir(p, sizeof(p)) || !gs_mkdirp(p)) return false;
    if (!tur_global_cache_dir(p, sizeof(p)) || !gs_mkdirp(p)) return false;
    if (!tur_global_bin_dir(p, sizeof(p)) || !gs_mkdirp(p)) return false;
    return true;
}

bool tur_global_bin_on_path(const char *bin_dir) {
    const char *path = getenv("PATH");
    if (!path || !bin_dir) return false;
    size_t blen = strlen(bin_dir);
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t seg = colon ? (size_t)(colon - p) : strlen(p);
        if (seg == blen && strncmp(p, bin_dir, blen) == 0) return true;
        if (!colon) break;
        p = colon + 1;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* state.tur reader/writer                                             */
/* ------------------------------------------------------------------ */

static const Form *gs_map_get_kw(const Form *map, const char *kw) {
    if (!map || map->tag != F_MAP) return NULL;
    const FormList *fl = &map->as.list;
    for (uint32_t i = 0; i + 1 < fl->len; i += 2) {
        const Form *key = fl->items[i];
        if (key->tag == F_KEYWORD && strcmp(key->as.sym->name, kw) == 0)
            return fl->items[i + 1];
    }
    return NULL;
}

static char *gs_form_str_dup(const Form *f) {
    if (!f || f->tag != F_STR) return NULL;
    return gs_ss_dup(f->as.s);
}

static void gs_parse_str_vec(const Form *f, char ***out, int *n_out) {
    *out = NULL;
    *n_out = 0;
    if (!f) return;
    if (f->tag != F_VEC && f->tag != F_LIST) return;
    const FormList *fl = &f->as.list;
    if (fl->len == 0) return;
    *out = (char **)malloc(fl->len * sizeof(char *));
    if (!*out) return;
    for (uint32_t i = 0; i < fl->len; i++) {
        if (fl->items[i]->tag == F_STR)
            (*out)[(*n_out)++] = gs_ss_dup(fl->items[i]->as.s);
    }
}

bool tur_state_read(const char *path, TurState *out) {
    memset(out, 0, sizeof(*out));
    out->format_version = 1;

    FILE *f = fopen(path, "rb");
    if (!f) return true; /* missing is OK */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return true; }
    char *src = (char *)malloc((size_t)sz + 1);
    if (!src) { fclose(f); return false; }
    if (fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(src); return false;
    }
    fclose(f);
    src[sz] = '\0';

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    char *src_arena = (char *)arena_alloc(&arena, (size_t)sz + 1);
    memcpy(src_arena, src, (size_t)sz + 1);
    free(src);
    src = src_arena;

    SourceFile file = {0};
    file.path = path;
    file.src  = src;
    file.len  = (uint32_t)sz;
    diag_register_file(&file);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);
    if (!forms || diag_had_error() || nforms == 0) {
        symtab_free(&st);
        arena_free(&arena);
        return true; /* empty / unparseable state -> treat as empty */
    }

    Form *ds = NULL;
    for (uint32_t i = 0; i < nforms; i++) {
        Form *frm = forms[i];
        if (frm->tag != F_LIST || frm->as.list.len < 1) continue;
        Form *head = frm->as.list.items[0];
        if (head->tag == F_SYM &&
            strcmp(head->as.sym->name, "defstate") == 0) {
            ds = frm;
            break;
        }
    }
    if (!ds) {
        symtab_free(&st);
        arena_free(&arena);
        return true;
    }

    const FormList *fl = &ds->as.list;
    for (uint32_t i = 1; i + 1 < fl->len; i += 2) {
        const Form *kf = fl->items[i];
        const Form *vf = fl->items[i + 1];
        if (kf->tag != F_KEYWORD) { i -= 1; continue; }
        const char *kw = kf->as.sym->name;
        if (strcmp(kw, "format-version") == 0) {
            if (vf->tag == F_INT) out->format_version = (int)vf->as.i;
        } else if (strcmp(kw, "installed") == 0) {
            if (!vf || vf->tag != F_MAP) continue;
            const FormList *mfl = &vf->as.list;
            for (uint32_t j = 0; j + 1 < mfl->len; j += 2) {
                const Form *nf = mfl->items[j];
                const Form *ef = mfl->items[j + 1];
                if (nf->tag != F_STR) continue;
                if (!ef || ef->tag != F_MAP) continue;

                out->entries = (TurStateEntry *)realloc(out->entries,
                    (out->n_entries + 1) * sizeof(TurStateEntry));
                if (!out->entries) {
                    symtab_free(&st); arena_free(&arena); return false;
                }
                TurStateEntry *e = &out->entries[out->n_entries++];
                memset(e, 0, sizeof(*e));
                e->name         = gs_ss_dup(nf->as.s);
                e->url          = gs_form_str_dup(gs_map_get_kw(ef, "url"));
                e->ref          = gs_form_str_dup(gs_map_get_kw(ef, "ref"));
                e->subdir       = gs_form_str_dup(gs_map_get_kw(ef, "subdir"));
                e->path         = gs_form_str_dup(gs_map_get_kw(ef, "path"));
                e->resolved     = gs_form_str_dup(gs_map_get_kw(ef, "resolved"));
                e->version      = gs_form_str_dup(gs_map_get_kw(ef, "version"));
                e->installed_at = gs_form_str_dup(gs_map_get_kw(ef, "installed-at"));
                e->built_with   = gs_form_str_dup(gs_map_get_kw(ef, "built-with"));
                gs_parse_str_vec(gs_map_get_kw(ef, "bin"),
                                 &e->bin, &e->n_bin);
            }
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    return true;
}

bool tur_state_write(const char *path, const TurState *st) {
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        fprintf(stderr, "tur: cannot write '%s': %s\n",
                path, strerror(errno));
        return false;
    }

    fprintf(f, ";;; state.tur -- installed-spice registry. Managed by `tur install`.\n");
    fprintf(f, "(defstate\n");
    fprintf(f, "  :format-version %d\n", st->format_version > 0 ? st->format_version : 1);
    fprintf(f, "  :installed #{\n");
    for (int i = 0; i < st->n_entries; i++) {
        const TurStateEntry *e = &st->entries[i];
        fprintf(f, "    \"%s\" #{", e->name ? e->name : "");
        if (e->url)          fprintf(f, ":url \"%s\" ", e->url);
        if (e->ref)          fprintf(f, ":ref \"%s\" ", e->ref);
        if (e->subdir)       fprintf(f, ":subdir \"%s\" ", e->subdir);
        if (e->path)         fprintf(f, ":path \"%s\" ", e->path);
        if (e->resolved)     fprintf(f, ":resolved \"%s\" ", e->resolved);
        if (e->version)      fprintf(f, ":version \"%s\" ", e->version);
        if (e->installed_at) fprintf(f, ":installed-at \"%s\" ", e->installed_at);
        if (e->built_with)   fprintf(f, ":built-with \"%s\" ", e->built_with);
        if (e->n_bin > 0) {
            fprintf(f, ":bin [");
            for (int j = 0; j < e->n_bin; j++) {
                if (j) fprintf(f, " ");
                fprintf(f, "\"%s\"", e->bin[j]);
            }
            fprintf(f, "] ");
        }
        fprintf(f, "}\n");
    }
    fprintf(f, "  })\n");

    fclose(f);
    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "tur: rename failed: %s\n", strerror(errno));
        unlink(tmp_path);
        return false;
    }
    return true;
}

static void gs_entry_free(TurStateEntry *e) {
    free(e->name);
    free(e->url);
    free(e->ref);
    free(e->subdir);
    free(e->path);
    free(e->resolved);
    free(e->version);
    free(e->installed_at);
    free(e->built_with);
    for (int j = 0; j < e->n_bin; j++) free(e->bin[j]);
    free(e->bin);
    memset(e, 0, sizeof(*e));
}

void tur_state_free(TurState *st) {
    for (int i = 0; i < st->n_entries; i++) gs_entry_free(&st->entries[i]);
    free(st->entries);
    memset(st, 0, sizeof(*st));
}

TurStateEntry *tur_state_find(TurState *st, const char *name) {
    for (int i = 0; i < st->n_entries; i++) {
        if (st->entries[i].name && strcmp(st->entries[i].name, name) == 0)
            return &st->entries[i];
    }
    return NULL;
}

TurStateEntry *tur_state_upsert(TurState *st, const char *name) {
    TurStateEntry *e = tur_state_find(st, name);
    if (e) {
        /* Wipe existing scalar fields; caller will re-populate. */
        free(e->url);          e->url = NULL;
        free(e->ref);          e->ref = NULL;
        free(e->subdir);       e->subdir = NULL;
        free(e->path);         e->path = NULL;
        free(e->resolved);     e->resolved = NULL;
        free(e->version);      e->version = NULL;
        free(e->installed_at); e->installed_at = NULL;
        free(e->built_with);   e->built_with = NULL;
        for (int j = 0; j < e->n_bin; j++) free(e->bin[j]);
        free(e->bin);
        e->bin = NULL;
        e->n_bin = 0;
        return e;
    }
    st->entries = (TurStateEntry *)realloc(st->entries,
        (st->n_entries + 1) * sizeof(TurStateEntry));
    if (!st->entries) return NULL;
    e = &st->entries[st->n_entries++];
    memset(e, 0, sizeof(*e));
    e->name = tur_strdup(name);
    return e;
}

bool tur_state_remove(TurState *st, const char *name) {
    for (int i = 0; i < st->n_entries; i++) {
        if (st->entries[i].name && strcmp(st->entries[i].name, name) == 0) {
            gs_entry_free(&st->entries[i]);
            for (int j = i; j < st->n_entries - 1; j++)
                st->entries[j] = st->entries[j + 1];
            st->n_entries--;
            return true;
        }
    }
    return false;
}

/* global-spice-library-consumption: resolve an INSTALLED spice's source
 * directory by name, for a manifest dep declared `#{:global true}`.
 *
 * Mirrors `tur list`'s reconstruction (install.c: inst_entry_src_dir): a
 * `--path` install points at the local checkout, a git install at
 * `spices/<name>-<sanitized-ref>` plus any `:subdir`.  Returns false when the
 * spice is not installed or its directory is gone, so callers can report
 * "install it first" rather than falling through to a project-local guess.
 *
 * `out_version` / `out_resolved`, when non-NULL, receive the state entry's
 * recorded version and resolved SHA (borrowed from the caller-owned state, so
 * they are copied out here).  A `--path` install has no SHA; *out_resolved is
 * left NULL then. */
bool tur_installed_spice_dir(const char *name, char *out, size_t out_sz,
                             char **out_version, char **out_resolved) {
    if (out_version)  *out_version  = NULL;
    if (out_resolved) *out_resolved = NULL;
    if (!name || !*name || !out || out_sz == 0) return false;

    char state_path[4096];
    if (!tur_global_state_path(state_path, sizeof(state_path))) return false;
    TurState st; memset(&st, 0, sizeof(st));
    if (!tur_state_read(state_path, &st)) return false;

    bool found = false;
    TurStateEntry *e = tur_state_find(&st, name);
    if (e) {
        char dir[4096];
        if (e->path) {
            snprintf(dir, sizeof(dir), "%s", e->path);
        } else {
            char spices_dir[4096];
            if (tur_global_spices_dir(spices_dir, sizeof(spices_dir))) {
                char ref_tag[128];
                snprintf(ref_tag, sizeof(ref_tag), "%s", e->ref ? e->ref : "HEAD");
                for (char *p = ref_tag; *p; p++) if (*p == '/') *p = '-';
                char base[4096];
                snprintf(base, sizeof(base), "%s/%s-%s", spices_dir, name, ref_tag);
                if (e->subdir) snprintf(dir, sizeof(dir), "%s/%s", base, e->subdir);
                else           snprintf(dir, sizeof(dir), "%s", base);
            } else {
                dir[0] = '\0';
            }
        }
        struct stat ds;
        if (dir[0] && stat(dir, &ds) == 0 && S_ISDIR(ds.st_mode)) {
            snprintf(out, out_sz, "%s", dir);
            if (out_version && e->version)   *out_version  = tur_strdup(e->version);
            if (out_resolved && e->resolved) *out_resolved = tur_strdup(e->resolved);
            found = true;
        }
    }
    tur_state_free(&st);
    return found;
}
