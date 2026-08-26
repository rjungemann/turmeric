#include "stdlib_autoload.h"

#include "diag.h"
#include "reader.h"
#include "runtime/experiments.h"
#include "runtime/globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Auto-loaded stdlib files.  Shared by compile_to_c (single-file) and
 * compile_to_h / compile_to_implementation (project-mode multi-file) so
 * spice code in `tur build .` sees `Cons`, `tnil?`, `Option`, etc. without
 * explicit imports -- matching single-file semantics.  Each TU embeds its
 * own static copy of stdlib defns; emit_module.c's
 * `is_from_stdlib`-aware paths static-ify them per TU so multi-TU links
 * don't see duplicate symbols. */
static const char *const autoload_files_[] = {
    "macros.tur",
    "safe.tur",
    "contract.tur",
    "hamt.tur",
    "typeclass-eq.tur",
    "typeclass-functor.tur",
    "typeclass-clone.tur",
    "typeclass-drop.tur",
    "typeclass-hash.tur",
    "typeclass-applicative.tur",
    "typeclass-alternative.tur",
    "typeclass-monad.tur",
    "typeclass-monaderror.tur",
    "typeclass-bifunctor.tur",
    "map.tur",
    "vec.tur",
    "slice.tur",
    "option.tur",
    "result.tur",
    "pair.tur",
    "tuple.tur",
    "list.tur",
    "grid.tur",
    "zipper.tur",
    "set.tur",
    "mutmap.tur",
    "json.tur",
    "schema.tur",
    "sym.tur",
    "unique.tur",
    NULL
};

/* SX1 (solver-extension-plan): stdlib/trail.tur is auto-loaded ONLY when the
 * `backtrackable-state` experiment is on.
 *
 * This is where the gate belongs for an experimental stdlib module.  Gating a
 * call site would mean the names exist and then refuse to work; gating the
 * autoload means that with the experiment off the module is simply not there,
 * and `bt-set!` is an unknown function like any other -- which is the honest
 * report.  With it on, the lifecycle warning fires once per compile, from the
 * one place that knows the feature was actually pulled in.
 *
 * The copy is rebuilt per call rather than cached: `tur repl` and the test
 * harnesses flip experiment bits between compiles in one process, and a cached
 * list would serve the first compile's answer to the second. */
const char *const *tur_stdlib_autoload_files(void) {
    if (!g_opt_backtrackable_state) return autoload_files_;

    static const char *gated_[sizeof(autoload_files_) / sizeof(autoload_files_[0]) + 1];
    size_t n = 0;
    for (; autoload_files_[n]; n++) gated_[n] = autoload_files_[n];
    gated_[n++] = "trail.tur";
    gated_[n]   = NULL;
    experiment_warn_if_used("backtrackable-state");
    return gated_;
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *back = strrchr(path, '\\');
    if (back && (!slash || back > slash)) slash = back;
#endif
    return slash ? slash + 1 : path;
}

/* Read a whole file without printing on failure -- a missing autoload file is
 * expected during --no-auto-stdlib builds and must not become stderr noise. */
static int read_file_quiet(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) { free(buf); return -1; }
    buf[size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

uint32_t tur_stdlib_prepend_forms(Arena *arena, SymbolTable *st,
                                  const char *stdlib_dir,
                                  const char *entry_path,
                                  bool no_auto_stdlib,
                                  Form ***forms_in_out,
                                  uint32_t *nforms_in_out,
                                  uint8_t *file_id_in_out) {
    if (!stdlib_dir || !*stdlib_dir) stdlib_dir = "stdlib";

    /* Through the accessor, not the raw array: that is where the SX1
     * experiment gate adds trail.tur, and reading `autoload_files_` directly
     * here meant the single-file path silently ignored it while project mode
     * honoured it -- the same program compiling two different ways. */
    const char *const *files = tur_stdlib_autoload_files();

    int no_stdlib_skip_from = -1;
    if (no_auto_stdlib) {
        const char *input_base = basename_of(entry_path);
        for (int j = 0; files[j] != NULL; j++) {
            if (strcmp(input_base, files[j]) == 0) {
                no_stdlib_skip_from = j;
                break;
            }
        }
    }

    uint32_t total = 0;
    Form **all = NULL;
    for (int i = 0; files[i] != NULL; i++) {
        if (no_stdlib_skip_from >= 0 && i >= no_stdlib_skip_from) continue;
        char path_buf[4096];
        int pn = snprintf(path_buf, sizeof(path_buf), "%s/%s",
                          stdlib_dir, files[i]);
        if (pn < 0 || (size_t)pn >= sizeof(path_buf)) {
            fprintf(stderr, "tur: stdlib path too long for '%s' (dir='%s')\n",
                    files[i], stdlib_dir);
            continue;
        }
        char *stdlib_src = NULL;
        size_t stdlib_len = 0;
        if (read_file_quiet(path_buf, &stdlib_src, &stdlib_len) != 0)
            continue;

        char *src_copy = (char *)arena_alloc(arena, stdlib_len);
        memcpy(src_copy, stdlib_src, stdlib_len);
        char *path_copy = (char *)arena_alloc(arena, strlen(path_buf) + 1);
        memcpy(path_copy, path_buf, strlen(path_buf) + 1);

        SourceFile *stdlib_file = (SourceFile *)arena_alloc(arena, sizeof(SourceFile));
        *stdlib_file = (SourceFile){0};
        stdlib_file->path = path_copy;
        stdlib_file->src = src_copy;
        stdlib_file->len = stdlib_len;
        stdlib_file->file_id = (*file_id_in_out)++;
        stdlib_file->reader_type = READER_TURMERIC;
        diag_register_file(stdlib_file);

        uint32_t n = 0;
        Form **fs = read_all(arena, st, stdlib_file, &n);
        if (fs && n > 0) {
            Form **new_all = (Form **)arena_alloc(arena,
                (total + n) * sizeof(Form *));
            for (uint32_t j = 0; j < total; j++) new_all[j] = all[j];
            for (uint32_t j = 0; j < n; j++) new_all[total + j] = fs[j];
            all = new_all;
            total += n;
        }
        free(stdlib_src);
    }

    if (all && total > 0) {
        Form **out = (Form **)arena_alloc(arena,
            (*nforms_in_out + total) * sizeof(Form *));
        for (uint32_t i = 0; i < total; i++) out[i] = all[i];
        for (uint32_t i = 0; i < *nforms_in_out; i++)
            out[total + i] = (*forms_in_out)[i];
        *forms_in_out = out;
        *nforms_in_out += total;
    }
    return total;
}
