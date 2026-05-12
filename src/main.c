/* tur: the Turmeric compiler driver (phase 2). */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* _DARWIN_C_SOURCE exposes mkstemps and DT_REG on macOS without suppressing
 * POSIX extensions; on Linux _DEFAULT_SOURCE covers both. */
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/wait.h>

#include "arena.h"
#include "buf.h"
#include "borrow_check.h"  /* Phase 14 */
#include "cps.h"          /* Phase 18: CPS transformation */
#include "diag.h"
#include "effect_check.h" /* Phase P19-2: effect-row inference */
#include "kind_check.h"   /* Phase HKT H0: kind inference pass */
#include "elab.h"
#include "emit.h"
#include "effect_lower.h" /* Phase 19: Effect lowering */
#include "expr.h"
#include "forms.h"
#include "pass.h"         /* Phase P19-1: pass scheduling */
#include "reader.h"
#include "symbols.h"

/* Extract basename from a path. */
static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Helper to detect language and adjust source for #lang directive */
static ReaderType detect_and_adjust_lang(const char *path, char *src, size_t len,
                                        const char **out_src, size_t *out_len) {
    const char *src_rest = src;
    size_t len_rest = len;
    ReaderType detected_type = reader_type_from_extension(path);
    
    /* Only parse #lang if file extension didn't already select sweet */
    if (detected_type == READER_TURMERIC) {
        detected_type = detect_lang(src, len, &src_rest, &len_rest);
    }
    
    /* Check if the reader is implemented */
    if (!reader_type_is_implemented(detected_type)) {
        fprintf(stderr, "tur: error: #lang %s is not yet implemented\n", 
                reader_type_name(detected_type));
        exit(1);
    }
    
    *out_src = src_rest;
    *out_len = len_rest;
    return detected_type;
}

static int read_entire_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "tur: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); fprintf(stderr, "tur: oom\n"); return -1; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return -1; }
    buf[size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

/* Ordered list of compiler passes executed for every compilation unit.
 * To insert a new pass: add a PassKind constant to pass.h, add a case here,
 * and add the case in run_core_passes() below.                              */
static const PassKind core_passes[] = {
    PASS_ELABORATE,
    PASS_KIND_CHECK,
    PASS_EFFECT_LOWER,
    PASS_EFFECT_ROW_INFER,  /* P19-2: stub slot — no-op until inference lands */
    PASS_CPS,
    PASS_BORROW_CHECK,
};
static const int n_core_passes = (int)(sizeof(core_passes) / sizeof(core_passes[0]));

/* Run all core compiler passes in order.  ctx->forms/nforms must be set
 * before calling; ctx->prog and ctx->effect_env are populated by the passes.
 * Returns 0 on success, 1 on the first pass failure.                        */
static int run_core_passes(PassContext *ctx) {
    for (int i = 0; i < n_core_passes; i++) {
        switch (core_passes[i]) {
        case PASS_ELABORATE:
            ctx->prog = elaborate_program(ctx->arena, ctx->st,
                                          ctx->forms, ctx->nforms);
            if (!ctx->prog || diag_had_error()) return 1;
            break;
        case PASS_KIND_CHECK:
            /* Phase HKT H0: kind inference and validation pass (v1 stub). */
            if (kind_check_pass(ctx->arena, ctx->prog) != 0)
                return 1;
            break;
        case PASS_EFFECT_LOWER:
            /* Phase 19: transform perform/handle into shift/reset. */
            ctx->effect_env = effect_env_new(ctx->arena);
            ctx->prog = effect_lower(ctx->arena, ctx->st,
                                     ctx->prog, ctx->effect_env);
            if (!ctx->prog || diag_had_error()) return 1;
            break;
        case PASS_EFFECT_ROW_INFER:
            /* P19-2: effect-row inference and validation pass. */
            if (effect_check_pass(ctx->arena, ctx->prog, ctx->effect_env) != 0)
                return 1;
            break;
        case PASS_CPS:
            /* Phase 18: CPS transformation for shift/reset. */
            ctx->prog = cps_transform(ctx->arena, ctx->prog);
            if (!ctx->prog || diag_had_error()) return 1;
            break;
        case PASS_BORROW_CHECK:
            /* Phase 14: ownership, move, and borrow analysis. */
            if (!borrow_check_program(ctx->prog)) return 1;
            /* Phase P19-7: Always-on check that handler case bodies do not
             * capture borrow-typed variables from the enclosing scope. */
            if (!borrow_check_effect_handler_captures(ctx->prog)) return 1;
            break;
        }
    }
    return 0;
}

/* Reads a .tur file and emits its C source into `out_c`. Returns 0 on success,
 * nonzero on error (diagnostics already emitted). */
static int compile_to_c(const char *path, Buf *out_c) {
    char  *src = NULL;
    size_t len = 0;
    if (read_entire_file(path, &src, &len) != 0) return 2;

    /* Detect language and adjust source for #lang directive */
    const char *src_adj = src;
    size_t len_adj = len;
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj);

    SourceFile file = {0};
    file.path = path;
    file.src = src_adj;
    file.len = len_adj;
    file.file_id = 0;
    file.reader_type = reader_type;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    /* Phase 7: Load standard library files */
    /* For now, load them in a specific order to ensure dependencies are met */
    /* Note: option, result, slice, str, vec, test use inline C with malloc/free
     * which causes type mismatches when compiled into every file.
     * They're deferred until Phase 11 when :ptr<T> support is added.
     * For Phase 7, we load only macros.tur which contains when/unless macros. */
    const char *stdlib_files[] = {
        "stdlib/macros.tur",
        /* "stdlib/typeclass.tur" loaded on demand via (require typeclass) - Phase 15 */
        /* Phase T19-C/D stdlib files (mutex, rwlock, condvar, sync, thread, chan,
         * atomic) are NOT auto-loaded here to avoid polluting every program's
         * generated C and invalidating codegen snapshots.  They are library files
         * usable via `tur build <dir>` when placed next to user code, matching the
         * pattern established by stdlib/atomic.tur.  An explicit `require` or
         * module mechanism (planned post-T21) will provide auto-loading later. */
        NULL
    };
    
    uint32_t total_stdlib_forms = 0;
    Form **all_stdlib_forms = NULL;
    uint8_t file_id = 1;
    
    for (int i = 0; stdlib_files[i] != NULL; i++) {
        char *stdlib_src = NULL;
        size_t stdlib_len = 0;
        if (read_entire_file(stdlib_files[i], &stdlib_src, &stdlib_len) == 0) {
            /* strdup the source so it lives in the arena and won't be freed prematurely */
            char *src_copy = (char *)arena_alloc(&arena, stdlib_len);
            memcpy(src_copy, stdlib_src, stdlib_len);
            
            /* Allocate a fresh SourceFile per stdlib file — each must have its
             * own stable arena address since diag and reader store pointers.  */
            SourceFile *stdlib_file = (SourceFile *)arena_alloc(&arena, sizeof(SourceFile));
            *stdlib_file = (SourceFile){0};
            stdlib_file->path = stdlib_files[i];
            stdlib_file->src = src_copy;
            stdlib_file->len = stdlib_len;
            stdlib_file->file_id = file_id++;
            stdlib_file->reader_type = READER_TURMERIC;  /* Stdlib is always plain s-exprs */
            diag_register_file(stdlib_file);

            uint32_t stdlib_nforms = 0;
            Form **stdlib_forms = read_all(&arena, &st, stdlib_file, &stdlib_nforms);

            if (stdlib_forms && stdlib_nforms > 0) {
                /* Append to all_stdlib_forms */
                Form **new_all = (Form **)arena_alloc(&arena, 
                    (total_stdlib_forms + stdlib_nforms) * sizeof(Form *));
                for (uint32_t j = 0; j < total_stdlib_forms; j++) {
                    new_all[j] = all_stdlib_forms[j];
                }
                for (uint32_t j = 0; j < stdlib_nforms; j++) {
                    new_all[total_stdlib_forms + j] = stdlib_forms[j];
                }
                all_stdlib_forms = new_all;
                total_stdlib_forms += stdlib_nforms;
            }
            free(stdlib_src);
        }
    }
    
    /* Prepend stdlib forms to user forms */
    if (all_stdlib_forms && total_stdlib_forms > 0) {
        Form **all_forms = (Form **)arena_alloc(&arena, 
            (nforms + total_stdlib_forms) * sizeof(Form *));
        for (uint32_t i = 0; i < total_stdlib_forms; i++) {
            all_forms[i] = all_stdlib_forms[i];
        }
        for (uint32_t i = 0; i < nforms; i++) {
            all_forms[total_stdlib_forms + i] = forms[i];
        }
        forms = all_forms;
        nforms += total_stdlib_forms;
    }

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        PassContext ctx = {0};
        ctx.arena = &arena;
        ctx.st    = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        rc = run_core_passes(&ctx);
        if (rc == 0 && emit_program(out_c, ctx.prog) != 0) {
            rc = 1;
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

/* Compile a .tur file to a C header (.h). Returns 0 on success. */
static int compile_to_h(const char *path, Buf *out_h, const char *module_name) {
    char  *src = NULL;
    size_t len = 0;
    if (read_entire_file(path, &src, &len) != 0) return 2;

    /* Detect language and adjust source for #lang directive */
    const char *src_adj = src;
    size_t len_adj = len;
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj);

    SourceFile file = {0};
    file.path = path;
    file.src = src_adj;
    file.len = len_adj;
    file.file_id = 0;
    file.reader_type = reader_type;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        PassContext ctx = {0};
        ctx.arena  = &arena;
        ctx.st     = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        rc = run_core_passes(&ctx);
        if (rc == 0 && emit_header(out_h, module_name, ctx.prog) != 0) {
            rc = 1;
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

/* Compile a .tur file to a C implementation (.c). Returns 0 on success. */
static int compile_to_implementation(const char *path, Buf *out_c, const char *module_name) {
    char  *src = NULL;
    size_t len = 0;
    if (read_entire_file(path, &src, &len) != 0) return 2;

    /* Detect language and adjust source for #lang directive */
    const char *src_adj = src;
    size_t len_adj = len;
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj);

    SourceFile file = {0};
    file.path = path;
    file.src = src_adj;
    file.len = len_adj;
    file.file_id = 0;
    file.reader_type = reader_type;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        PassContext ctx = {0};
        ctx.arena  = &arena;
        ctx.st     = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        rc = run_core_passes(&ctx);
        if (rc == 0 && emit_implementation(out_c, module_name, ctx.prog) != 0) {
            rc = 1;
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

/* Generate _main.c that includes all .h files and has main(). */
static int generate_main_c(Buf *out, const char **h_files, int n_files, const char *output_name) {
    buf_printf(out, "/* generated by tur (phase 2) - _main.c */\n");
    buf_puts(out, "#include <stdio.h>\n");
    buf_puts(out, "#include <stdint.h>\n");
    buf_puts(out, "#include <stdbool.h>\n\n");

    for (int i = 0; i < n_files; i++) {
        buf_printf(out, "#include \"%s\"\n", h_files[i]);
    }
    buf_puts(out, "\n");

    /* For now, just declare main in _main.c if no module has it.
     * In the future, we'll detect which module has main and only include that.
     * For phase 2, we assume the user provides main in one of the modules. */
    buf_puts(out, "/* main() should be defined in one of the included modules */\n");
    return 0;
}

static int cmd_emit_c(const char *path) {
    Buf out;
    buf_init(&out);
    int rc = compile_to_c(path, &out);
    if (rc == 0) buf_to_file(&out, stdout);
    buf_free(&out);
    return rc;
}

/* Choose an output executable name from the input path: foo.tur -> foo. */
static void default_output_name(const char *input, char *out, size_t cap) {
    const char *base = basename_of(input);
    size_t n = strlen(base);
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
    /* Only strip extension if this looks like a file (has a dot that's not at the start) */
    if (n > 0 && out[n-1] != '/') {
        char *dot = strrchr(out, '.');
        if (dot && dot != out) { *dot = '\0'; }
    }
}

static int cmd_build(const char *input, const char *out_path);

/* Build a deterministic path for the intermediate generated-C file so that
 * ccache can cache repeated compilations of the same .tur source.
 * Maps the input path to /tmp/tur-build/<sanitized>.c where non-alphanumeric
 * characters (except '-') are replaced with '_'.
 * Creates /tmp/tur-build/ on first call. */
static void stable_c_path(const char *input, char *out, size_t cap) {
    static int dir_made = 0;
    if (!dir_made) { mkdir("/tmp/tur-build", 0700); dir_made = 1; }
    const char *prefix = "/tmp/tur-build/";
    size_t plen = strlen(prefix);
    if (plen + 3 >= cap) { out[0] = '\0'; return; }
    memcpy(out, prefix, plen);
    size_t i = plen;
    for (const char *p = input; *p && i < cap - 3; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-') {
            out[i++] = c;
        } else {
            out[i++] = '_';
        }
    }
    out[i++] = '.';
    out[i++] = 'c';
    out[i] = '\0';
}

static int cmd_build(const char *input, const char *out_path) {
    Buf csrc;
    buf_init(&csrc);
    int rc = compile_to_c(input, &csrc);
    if (rc != 0) { buf_free(&csrc); return rc; }

    /* Write generated C to a deterministic path so ccache can cache the result
     * across repeated builds of the same .tur file.  Fall back to a random
     * temp if the stable path cannot be constructed (e.g. path too long). */
    char tmpl[1024];
    stable_c_path(input, tmpl, sizeof(tmpl));
    FILE *tf = tmpl[0] ? fopen(tmpl, "wb") : NULL;
    if (!tf) {
        /* fallback: random temp */
        char fallback[] = "/tmp/tur-XXXXXX.c";
        int fd = mkstemps(fallback, 2);
        if (fd < 0) {
            fprintf(stderr, "tur: cannot create temp file\n");
            buf_free(&csrc);
            return 2;
        }
        tf = fdopen(fd, "wb");
        memcpy(tmpl, fallback, sizeof(fallback));
    }
    if (!tf || fwrite(csrc.data, 1, csrc.len, tf) != csrc.len) {
        fprintf(stderr, "tur: write failed\n");
        if (tf) fclose(tf);
        buf_free(&csrc);
        return 2;
    }
    fclose(tf);
    buf_free(&csrc);

    char chosen_out[1024];
    if (!out_path) {
        default_output_name(input, chosen_out, sizeof(chosen_out));
        out_path = chosen_out;
    }

    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";

    /* TUR_CC_FLAGS overrides the default compiler flags.  Useful for test runs
     * where -O0 is fast enough and ccache benefits from consistent flags. */
    const char *cc_flags = getenv("TUR_CC_FLAGS");
    if (!cc_flags || !*cc_flags) cc_flags = "-O2 -std=c99 -Wall";

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s %s -o %s %s", cc, cc_flags, out_path, tmpl);
    int sys_rc = system(cmd.data);
    buf_free(&cmd);
    /* Leave the stable temp file for ccache; only unlink random fallbacks. */
    if (tmpl[0] == '/' && strncmp(tmpl, "/tmp/tur-build/", 15) != 0) {
        unlink(tmpl);
    }

    if (sys_rc != 0) {
        fprintf(stderr, "tur: cc invocation failed (status %d)\n", sys_rc);
        return 2;
    }
    return 0;
}

static int cmd_run(const char *input) {
    char out_path[] = "/tmp/tur-run-XXXXXX";
    int fd = mkstemp(out_path);
    if (fd < 0) { fprintf(stderr, "tur: mkstemp failed\n"); return 2; }
    close(fd);
    /* We need a path mkstemp picked, but cc will overwrite it; that's fine. */

    int rc = cmd_build(input, out_path);
    if (rc != 0) { unlink(out_path); return rc; }

    int sys_rc = system(out_path);
    unlink(out_path);
    if (sys_rc != 0) return sys_rc;
    return 0;
}

/* Collect all .tur files in a directory. Returns malloc'd array, sets *n_out. */
static char **collect_tur_files(const char *dir, int *n_out) {
    DIR *d = opendir(dir);
    if (!d) return NULL;

    char **files = NULL;
    int cap = 0;
    int n = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_type != DT_REG) continue;
        size_t len = strlen(ent->d_name);
        if (len >= 4 && strcmp(ent->d_name + len - 4, ".tur") == 0) {
            if (n >= cap) {
                cap = cap ? cap * 2 : 8;
                files = (char **)realloc(files, cap * sizeof(char *));
            }
            files[n] = (char *)malloc(strlen(dir) + 1 + len + 1);
            snprintf(files[n], strlen(dir) + 1 + len + 1, "%s/%s", dir, ent->d_name);
            n++;
        }
    }
    closedir(d);
    *n_out = n;
    return files;
}

/* Free the array returned by collect_tur_files. */
static void free_tur_files(char **files, int n) {
    for (int i = 0; i < n; i++) free(files[i]);
    free(files);
}

static int compare_cstr_ptrs(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static int decode_exit_status(int status) {
    if (status == -1) return 127;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int cmd_test(const char *dir) {
    int n_files = 0;
    char **tur_files = collect_tur_files(dir, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr, "tur: no .tur files found in '%s'\n", dir);
        free_tur_files(tur_files, n_files);
        return 1;
    }

    qsort(tur_files, (size_t)n_files, sizeof(char *), compare_cstr_ptrs);

    int passed = 0;
    int failed = 0;
    char **failed_files = (char **)calloc((size_t)n_files, sizeof(char *));
    if (!failed_files) {
        fprintf(stderr, "tur: oom\n");
        free_tur_files(tur_files, n_files);
        return 2;
    }

    for (int i = 0; i < n_files; i++) {
        char out_path[] = "/tmp/tur-test-XXXXXX";
        int fd = mkstemp(out_path);
        if (fd < 0) {
            fprintf(stderr, "tur: mkstemp failed for %s\n", tur_files[i]);
            failed_files[failed++] = tur_files[i];
            putchar('F');
            continue;
        }
        close(fd);

        int build_rc = cmd_build(tur_files[i], out_path);
        int run_rc = 1;
        if (build_rc == 0) {
            int status = system(out_path);
            run_rc = decode_exit_status(status);
        }
        unlink(out_path);

        if (build_rc == 0 && run_rc == 0) {
            passed++;
            putchar('.');
        } else {
            failed_files[failed++] = tur_files[i];
            putchar('F');
        }
    }

    putchar('\n');
    printf("%d tests, %d passed, %d failed\n", n_files, passed, failed);
    for (int i = 0; i < failed; i++) {
        printf("FAIL %s\n", failed_files[i]);
    }

    free(failed_files);
    free_tur_files(tur_files, n_files);
    return failed == 0 ? 0 : 1;
}

/* Build a project from multiple .tur files. Generates .h and .c for each,
 * plus a _main.c that includes all headers. */
static int cmd_build_multi(const char *dir, const char *out_path) {
    int n_files;
    char **tur_files = collect_tur_files(dir, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr, "tur: no .tur files found in '%s'\n", dir);
        free_tur_files(tur_files, n_files);
        return 1;
    }

    char chosen_out[1024];
    if (!out_path) {
        default_output_name(dir, chosen_out, sizeof(chosen_out));
        out_path = chosen_out;
    }

    /* Allocate arrays for .h and .c filenames */
    char **h_files = (char **)malloc(n_files * sizeof(char *));
    char **c_files = (char **)malloc(n_files * sizeof(char *));
    if (!h_files || !c_files) { fprintf(stderr, "tur: oom\n"); return 2; }

    /* Generate module names (basename without .tur) */
    for (int i = 0; i < n_files; i++) {
        const char *base = basename_of(tur_files[i]);
        size_t len = strlen(base);
        h_files[i] = (char *)malloc(len + 4);
        c_files[i] = (char *)malloc(len + 4);
        snprintf(h_files[i], len + 4, "%.*s.h", (int)(len - 4), base);
        snprintf(c_files[i], len + 4, "%.*s.c", (int)(len - 4), base);
    }

    /* Compile each .tur file to .h and .c */
    for (int i = 0; i < n_files; i++) {
        const char *module_name = basename_of(tur_files[i]);
        size_t len = strlen(module_name);
        char mod_name_no_ext[256];
        snprintf(mod_name_no_ext, sizeof(mod_name_no_ext), "%.*s", (int)(len - 4), module_name);

        Buf h_out;
        buf_init(&h_out);
        if (compile_to_h(tur_files[i], &h_out, mod_name_no_ext) != 0) {
            fprintf(stderr, "tur: failed to compile %s to header\n", tur_files[i]);
            buf_free(&h_out);
            for (int j = 0; j < i; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        if (buf_to_path(&h_out, h_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s\n", h_files[i]);
            buf_free(&h_out);
            for (int j = 0; j < i; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&h_out);

        Buf c_out;
        buf_init(&c_out);
        if (compile_to_implementation(tur_files[i], &c_out, mod_name_no_ext) != 0) {
            fprintf(stderr, "tur: failed to compile %s to implementation\n", tur_files[i]);
            buf_free(&c_out);
            for (int j = 0; j < i; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        if (buf_to_path(&c_out, c_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s\n", c_files[i]);
            buf_free(&c_out);
            for (int j = 0; j < i; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&c_out);
    }

    /* Generate _main.c */
    Buf main_c;
    buf_init(&main_c);
    if (generate_main_c(&main_c, (const char **)h_files, n_files, out_path) != 0) {
        fprintf(stderr, "tur: failed to generate _main.c\n");
        buf_free(&main_c);
        for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); }
        free(h_files); free(c_files);
        free_tur_files(tur_files, n_files);
        return 1;
    }
    if (buf_to_path(&main_c, "_main.c") != 0) {
        fprintf(stderr, "tur: failed to write _main.c\n");
        buf_free(&main_c);
        for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); }
        free(h_files); free(c_files);
        free_tur_files(tur_files, n_files);
        return 1;
    }
    buf_free(&main_c);

    /* Compile everything together with cc */
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";

    const char *cc_flags = getenv("TUR_CC_FLAGS");
    if (!cc_flags || !*cc_flags) cc_flags = "-O2 -std=c99 -Wall";

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s %s -o %s", cc, cc_flags, out_path);
    /* Add _main.c first */
    buf_printf(&cmd, " _main.c");
    /* Add all .c files */
    for (int i = 0; i < n_files; i++) {
        buf_printf(&cmd, " %s", c_files[i]);
    }
    int sys_rc = system(cmd.data);
    buf_free(&cmd);

    /* Clean up temp files */
    for (int i = 0; i < n_files; i++) { free(h_files[i]); free(c_files[i]); }
    free(h_files); free(c_files);
    free_tur_files(tur_files, n_files);
    unlink("_main.c");

    if (sys_rc != 0) {
        fprintf(stderr, "tur: cc invocation failed (status %d)\n", sys_rc);
        return 2;
    }
    return 0;
}

static int is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static int usage(void) {
    fprintf(stderr,
        "tur: the Turmeric compiler (phase 8)\n"
        "\n"
        "usage:\n"
        "  tur build <file.tur> [-o <out>]    build a single file\n"
        "  tur build <dir> [-o <out>]         build all .tur files in directory\n"
        "  tur emit-c <input.tur>            print the generated C to stdout\n"
        "  tur run <input.tur>               build + execute a single file\n"
        "  tur test <dir>                    run all .tur files in a directory\n"
        "  tur check <input.tur>             type-check only, no codegen (phase 8)\n"
        "\n"
        "global flags:\n"
        "  --no-color                       disable colored diagnostics\n"
        "  --json-diagnostics               output diagnostics as JSON (phase 8)\n"
        "  --explain <code>                 compile code snippet and explain errors (phase 8)\n");
    return 64;
}

/* Phase 8: Handle --no-color flag */
static bool parse_no_color(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-color") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase 8: Handle --explain flag - compile code snippet and show detailed error */
static int cmd_explain(const char *code) {
    /* Create a temporary source file from the code snippet */
    SourceFile file = {0};
    file.path = "<explain>";
    file.src = code;
    file.len = strlen(code);
    file.file_id = 0;
    file.reader_type = READER_TURMERIC;  /* Default for explain snippets */
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    if (!forms || diag_had_error()) {
        /* Error already emitted with enhanced diagnostics */
        symtab_free(&st);
        arena_free(&arena);
        return 1;
    }

    Expr *prog = elaborate_program(&arena, &st, forms, nforms);
    if (!prog || diag_had_error()) {
        /* Error already emitted */
        symtab_free(&st);
        arena_free(&arena);
        return 1;
    }

    /* If no error, just say so */
    fprintf(stderr, "No errors found in the provided code.\n");

    symtab_free(&st);
    arena_free(&arena);
    return 0;
}

/* Phase 8: Handle --json-diagnostics flag */
static bool use_json_diagnostics = false;

int main(int argc, char **argv) {
    /* Phase 8: Check for global flags before command */
    bool no_color = parse_no_color(argc, argv);
    bool explain_mode = false;
    const char *explain_code = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json-diagnostics") == 0) {
            use_json_diagnostics = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--explain") == 0 && i + 1 < argc) {
            explain_mode = true;
            explain_code = argv[i + 1];
            /* Remove --explain and code from argv */
            for (int j = i; j < argc - 2; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        }
    }
    
        /* Initialize diagnostics - use color unless --no-color or --json-diagnostics specified */
        /* JSON output disables color */
        diag_init(!no_color && !use_json_diagnostics && stderr_is_tty());
        diag_set_json_output(use_json_diagnostics);
    
    if (explain_mode) {
        if (!explain_code) {
            fprintf(stderr, "tur: --explain requires a code snippet argument\n");
            return usage();
        }
        return cmd_explain(explain_code);
    }
    
    if (argc < 2) return usage();
    const char *cmd = argv[1];

    if (strcmp(cmd, "emit-c") == 0) {
        if (argc != 3) return usage();
        return cmd_emit_c(argv[2]);
    }
    if (strcmp(cmd, "check") == 0) {
        /* Phase 8: tur check subcommand - type-check only, no codegen */
        if (argc != 3) return usage();
        Buf out;
        buf_init(&out);
        int rc = compile_to_c(argv[2], &out);
        buf_free(&out);
        return rc;
    }
    if (strcmp(cmd, "build") == 0) {
        const char *input = NULL;
        const char *out = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (argv[i][0] != '-') {
                if (input) return usage();
                input = argv[i];
            } else {
                return usage();
            }
        }
        if (!input) return usage();
        /* Check if input is a directory - use multi-file build */
        if (is_directory(input)) {
            return cmd_build_multi(input, out);
        } else {
            return cmd_build(input, out);
        }
    }
    if (strcmp(cmd, "run") == 0) {
        if (argc != 3) return usage();
        return cmd_run(argv[2]);
    }
    if (strcmp(cmd, "test") == 0) {
        if (argc != 3) return usage();
        return cmd_test(argv[2]);
    }
    return usage();
}
