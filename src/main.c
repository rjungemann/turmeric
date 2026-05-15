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
#include "assert.h"
#include "buf.h"
#include "borrow_check.h"  /* Phase 14 */
#include "cps.h"          /* Phase 18: CPS transformation */
#include "diag.h"
#include "effect_check.h" /* Phase P19-2: effect-row inference */
#include "effect.h"       /* built-in effect registration */
#include "kind_check.h"   /* Phase HKT H0: kind inference pass */
#include "elab.h"
#include "emit.h"
#include "effect_lower.h" /* Phase 19: Effect lowering */
#include "expr.h"
#include "fmt.h"
#include "forms.h"
#include "pass.h"         /* Phase P19-1: pass scheduling */
#include "reader.h"
#include "symbols.h"
/* Phase S0: eval API for tur repl */
#include "turi/eval.h"
/* Phase S1: REPL with libedit, multi-line input, :type/:doc/:reload */
#include "turi/repl.h"
/* Phase PKG-1: Spice package manager */
#include "pkg.h"
/* Global configuration variables — defined in globals.c */
#include "globals.h"

/* Extract basename from a path. */
static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Global configuration variables — defined in globals.c (part of tur_core) */
/* Phase HKT-P6: --dump-kinds flag: print kind annotations after kind-check */
static bool g_dump_kinds = false;

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

/* Compute the directory part of a file path (Phase M2: module base dir). */
static void dir_of_path(const char *path, char *out, size_t cap) {
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        out[0] = '.'; out[1] = '\0';
    } else {
        size_t n = (size_t)(last_slash - path);
        if (n == 0) n = 1; /* handle "/foo" → "/" */
        if (n >= cap) n = cap - 1;
        memcpy(out, path, n);
        out[n] = '\0';
    }
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
                                          ctx->forms, ctx->nforms,
                                          ctx->stdlib_prefix,
                                          ctx->module_base_dir,
                                          ctx->separate_compilation,
                                          &ctx->tc_env,
                                          ctx->include_dirs,
                                          ctx->n_include_dirs);
            if (!ctx->prog || diag_had_error()) return 1;
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info is preserved after elaboration */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_ELABORATE");
#endif
            break;
        case PASS_KIND_CHECK:
            /* Phase HKT H0: kind inference and validation pass (v1 stub). */
            if (kind_check_pass(ctx->arena, ctx->prog) != 0)
                return 1;
            /* Phase HKT-P6: optionally dump kind annotations for debugging. */
            if (g_dump_kinds)
                kind_dump_program(ctx->prog, stdout);
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info is preserved after kind-check */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_KIND_CHECK");
#endif
            break;
        case PASS_EFFECT_LOWER:
            /* Phase 19: transform perform/handle into shift/reset. */
            ctx->effect_env = effect_env_new(ctx->arena);
            effect_env_register_builtin_unsafe(
                ctx->effect_env, ctx->arena,
                symtab_intern(ctx->st, strslice(EFFECT_NAME_UNSAFE, 6)));
            ctx->prog = effect_lower(ctx->arena, ctx->st,
                                     ctx->prog, ctx->effect_env);
            if (!ctx->prog || diag_had_error()) return 1;
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after effect-lower */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_EFFECT_LOWER");
#endif
            break;
        case PASS_EFFECT_ROW_INFER:
            /* P19-2: effect-row inference and validation pass. */
            if (effect_check_pass(ctx->arena, ctx->prog, ctx->effect_env) != 0)
                return 1;
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after effect-row-infer */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_EFFECT_ROW_INFER");
#endif
            break;
        case PASS_CPS:
            /* Phase 18: CPS transformation for shift/reset. */
            ctx->prog = cps_transform(ctx->arena, ctx->prog, &ctx->tc_env);
            if (!ctx->prog || diag_had_error()) return 1;
            /* Phase B5: --dump-clone-plan: print cloneable capture plan after CPS */
            if (g_dump_clone_plan) cps_dump_clone_plan(ctx->prog, stderr);
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after CPS */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_CPS");
#endif
            break;
        case PASS_BORROW_CHECK:
            /* Phase 14: ownership, move, and borrow analysis. */
            if (!borrow_check_program(ctx->prog)) return 1;
            /* Phase P19-7: Always-on check that handler case bodies do not
             * capture borrow-typed variables from the enclosing scope. */
            if (!borrow_check_effect_handler_captures(ctx->prog)) return 1;
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after borrow-check */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_BORROW_CHECK");
#endif
            break;
        }
    }
    return 0;
}

/* Reads a .tur file and emits its C source into `out_c`. Returns 0 on success,
 * nonzero on error (diagnostics already emitted).
 * include_dirs/n_include_dirs: additional module search paths for (import ...). */
static int compile_to_c(const char *path, Buf *out_c,
                         const char **include_dirs, int n_include_dirs) {
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
        "stdlib/safe.tur",
        /* Phase C1: runtime contracts - auto-load contract.tur for assert!/require!/ensure!/invariant! */
        "stdlib/contract.tur",
        /* Phase P3: HAMT lowering - auto-load hamt.tur and map.tur */
        "stdlib/hamt.tur",
        "stdlib/map.tur",
        /* "stdlib/vec.tur" - has typeclass dependencies, not auto-loaded */
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
        char base_dir[4096];
        dir_of_path(path, base_dir, sizeof(base_dir));
        PassContext ctx = {0};
        ctx.arena = &arena;
        ctx.st    = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        ctx.stdlib_prefix = total_stdlib_forms;
        ctx.module_base_dir = base_dir;
        ctx.include_dirs    = include_dirs;
        ctx.n_include_dirs  = n_include_dirs;
        rc = run_core_passes(&ctx);
        if (rc == 0 && emit_program(out_c, ctx.prog) != 0) {
            rc = 1;
        }
    }

    /* Phase U5: Print unsafe statistics if enabled */
    if (g_unsafe_stats_enabled) {
        fprintf(stderr, "unsafe stats: %u blocks, %u total forms\n",
                g_unsafe_block_count, g_unsafe_total_lines);
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
        char base_dir[4096];
        dir_of_path(path, base_dir, sizeof(base_dir));
        PassContext ctx = {0};
        ctx.arena  = &arena;
        ctx.st     = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        ctx.module_base_dir = base_dir;
        ctx.separate_compilation = true;
        rc = run_core_passes(&ctx);
        if (rc == 0 && emit_header(out_h, module_name, ctx.prog, true) != 0) {
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
        char base_dir[4096];
        dir_of_path(path, base_dir, sizeof(base_dir));
        PassContext ctx = {0};
        ctx.arena  = &arena;
        ctx.st     = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        ctx.module_base_dir = base_dir;
        ctx.separate_compilation = true;
        rc = run_core_passes(&ctx);
        if (rc == 0 && emit_implementation(out_c, module_name, ctx.prog, true) != 0) {
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
    int rc = compile_to_c(path, &out, NULL, 0);
    if (rc == 0) buf_to_file(&out, stdout);
    buf_free(&out);
    return rc;
}

/* Phase B: emit per-module .h and .c files to a directory.
 * Usage: tur emit-c --output-dir <dir> <file1.tur> [<file2.tur> ...]
 * Each input produces <dir>/<module>.h and <dir>/<module>.c. */
static int cmd_emit_c_to_dir(const char *out_dir, char **inputs, int n_inputs) {
    if (n_inputs < 1) {
        fprintf(stderr, "tur emit-c --output-dir: at least one input required\n");
        return 1;
    }

    /* Create output directory if it doesn't exist */
    struct stat st;
    if (stat(out_dir, &st) != 0) {
        if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "tur emit-c: cannot create '%s': %s\n",
                    out_dir, strerror(errno));
            return 2;
        }
    }

    int rc = 0;
    for (int i = 0; i < n_inputs && rc == 0; i++) {
        const char *input = inputs[i];
        const char *base = basename_of(input);
        size_t base_len = strlen(base);
        char mod_name[256];
        size_t n = (base_len >= 4 && strcmp(base + base_len - 4, ".tur") == 0)
                   ? base_len - 4 : base_len;
        if (n >= sizeof(mod_name)) n = sizeof(mod_name) - 1;
        memcpy(mod_name, base, n);
        mod_name[n] = '\0';

        char h_path[1024], c_path[1024];
        snprintf(h_path, sizeof(h_path), "%s/%s.h", out_dir, mod_name);
        snprintf(c_path, sizeof(c_path), "%s/%s.c", out_dir, mod_name);

        Buf h_buf, c_buf;
        buf_init(&h_buf);
        buf_init(&c_buf);

        int h_rc = compile_to_h(input, &h_buf, mod_name);
        int c_rc = compile_to_implementation(input, &c_buf, mod_name);

        if (h_rc != 0 || c_rc != 0) {
            rc = (h_rc != 0) ? h_rc : c_rc;
        } else if (buf_to_path(&h_buf, h_path) != 0 ||
                   buf_to_path(&c_buf, c_path) != 0) {
            fprintf(stderr, "tur emit-c: failed to write output for '%s'\n",
                    input);
            rc = 2;
        }
        buf_free(&h_buf);
        buf_free(&c_buf);
    }
    return rc;
}

/* Phase M3: emit the .h for a single module file to stdout. */
static int cmd_emit_h(const char *path) {
    const char *base = basename_of(path);
    size_t base_len = strlen(base);
    char mod_name[256];
    size_t n = (base_len >= 4 && strcmp(base + base_len - 4, ".tur") == 0)
               ? base_len - 4 : base_len;
    if (n >= sizeof(mod_name)) n = sizeof(mod_name) - 1;
    memcpy(mod_name, base, n);
    mod_name[n] = '\0';

    Buf out;
    buf_init(&out);
    int rc = compile_to_h(path, &out, mod_name);
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

static int cmd_build(const char *input, const char *out_path,
                     const char **include_dirs, int n_include_dirs);
static char *find_project_root(const char *start);

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

static int cmd_build(const char *input, const char *out_path,
                     const char **include_dirs, int n_include_dirs) {
    Buf csrc;
    buf_init(&csrc);
    int rc = compile_to_c(input, &csrc, include_dirs, n_include_dirs);
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
    /* Phase S2: scan for __tur_include__ directives and inject them at the
     * top of the generated C so stdlib modules can add file-level includes.
     * The marker format is: slash-star __tur_include__: LINE star-slash */
    {
        const char *inc_mark = "/* __tur_include__: ";
        size_t inc_mlen = strlen(inc_mark);
        const char *p = csrc.data;
        Buf hdr;
        buf_init(&hdr);
        while (p && (p = strstr(p, inc_mark)) != NULL) {
            p += inc_mlen;
            const char *end = strstr(p, " */");
            if (!end) break;
            buf_write(&hdr, p, (size_t)(end - p));
            buf_putc(&hdr, '\n');
            p = end + 3;
        }
        if (hdr.len > 0) {
            Buf new_csrc;
            buf_init(&new_csrc);
            buf_write(&new_csrc, hdr.data, hdr.len);
            buf_write(&new_csrc, csrc.data, csrc.len);
            buf_free(&csrc);
            csrc = new_csrc;
        }
        buf_free(&hdr);
    }

    if (!tf || fwrite(csrc.data, 1, csrc.len, tf) != csrc.len) {
        fprintf(stderr, "tur: write failed\n");
        if (tf) fclose(tf);
        buf_free(&csrc);
        return 2;
    }
    fclose(tf);

    /* Phase S2: scan generated C for __tur_autolink__ comments and collect
     * any linker flags they embed (e.g. -lturi from stdlib/turi/eval.tur).
     * The marker format is: slash-star __tur_autolink__: FLAGS star-slash */
    Buf autolink;
    buf_init(&autolink);
    {
        const char *marker = "/* __tur_autolink__: ";
        size_t mlen = strlen(marker);
        const char *p = csrc.data;
        while (p && (p = strstr(p, marker)) != NULL) {
            p += mlen;
            const char *end = strstr(p, " */");
            if (!end) break;
            if (autolink.len > 0) buf_putc(&autolink, ' ');
            buf_write(&autolink, p, (size_t)(end - p));
            p = end + 3;
        }
        if (autolink.len > 0) buf_putc(&autolink, '\0');
    }
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

    /* Collect cmake dep flags from cmake/spice-deps-manifest.json if present */
    Buf cmake_flags;
    buf_init(&cmake_flags);
    {
        /* Walk up from the input file's directory to find project root */
        char input_dir[4096];
        strncpy(input_dir, input, sizeof(input_dir) - 1);
        input_dir[sizeof(input_dir) - 1] = '\0';
        char *slash = strrchr(input_dir, '/');
        if (slash) *slash = '\0';
        else strncpy(input_dir, ".", sizeof(input_dir));
        char *proj_root = find_project_root(input_dir);
        if (proj_root) {
            char manifest_path[4096];
            snprintf(manifest_path, sizeof(manifest_path),
                     "%s/cmake/spice-deps-manifest.json", proj_root);
            PkgCmakeManifest cmake_manifest;
            if (pkg_cmake_manifest_read(manifest_path, &cmake_manifest)) {
                pkg_cmake_manifest_append_cc_flags(&cmake_manifest, &cmake_flags);
                pkg_cmake_manifest_free(&cmake_manifest);
            }
            free(proj_root);
        }
    }

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s %s -o %s %s", cc, cc_flags, out_path, tmpl);
    /* Append any __tur_autolink__ flags discovered in the generated C. */
    if (autolink.len > 0) buf_printf(&cmd, " %s", autolink.data);
    buf_free(&autolink);
    /* Append cmake dep flags (-I/-L/-l). */
    if (cmake_flags.len > 0) buf_puts(&cmd, cmake_flags.data);
    buf_free(&cmake_flags);
    /* Append spice include dirs (-I). */
    for (int _i = 0; _i < n_include_dirs; _i++) {
        if (include_dirs[_i] && include_dirs[_i][0])
            buf_printf(&cmd, " -I%s", include_dirs[_i]);
    }
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

/* Walk up from 'start' to find a directory containing build.tur.
 * Returns malloc'd path string or NULL if not found. Caller must free(). */
static char *find_project_root(const char *start) {
    char dir[4096];
    strncpy(dir, start, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    for (;;) {
        char candidate[4096];
        snprintf(candidate, sizeof(candidate), "%s/build.tur", dir);
        struct stat st;
        if (stat(candidate, &st) == 0) {
            char *res = (char *)malloc(strlen(dir) + 1);
            if (res) strcpy(res, dir);
            return res;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = '\0';
    }
    return NULL;
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

static int cmd_run(int argc, char **argv) {
    /* tur run [--release] [--offline] [<file>] [-- <args>...] */
    bool        release           = false;
    bool        offline           = false;
    const char *explicit_file     = NULL;
    int         passthrough_start = -1;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--release") == 0) {
            release = true;
        } else if (strcmp(argv[i], "--offline") == 0) {
            offline = true;
        } else if (strcmp(argv[i], "--") == 0) {
            passthrough_start = i + 1;
            break;
        } else if (argv[i][0] != '-') {
            if (!explicit_file) explicit_file = argv[i];
        }
    }
    (void)release; /* passed to compiler when --release build is supported */

    /* spice_inc_dirs: populated below during project-mode setup.
     * RUN_ENTRY captures these via the enclosing scope. */
    const char **spice_inc_dirs = NULL;
    int          n_spice_inc_dirs = 0;

    /* Helper: build 'entry', exec with optional passthrough args. */
#define RUN_ENTRY(entry_path) do {                                       \
        char out_path[] = "/tmp/tur-run-XXXXXX";                         \
        int _fd = mkstemp(out_path);                                     \
        if (_fd < 0) { fprintf(stderr, "tur: mkstemp failed\n"); return 2; } \
        close(_fd);                                                      \
        int _rc = cmd_build((entry_path), out_path,                      \
                            spice_inc_dirs, n_spice_inc_dirs);           \
        if (_rc != 0) { unlink(out_path); free(spice_inc_dirs); return _rc; } \
        Buf _cmd; buf_init(&_cmd);                                       \
        buf_printf(&_cmd, "'%s'", out_path);                             \
        if (passthrough_start >= 0) {                                    \
            for (int _i = passthrough_start; _i < argc; _i++)           \
                buf_printf(&_cmd, " '%s'", argv[_i]);                   \
        }                                                                \
        buf_putc(&_cmd, '\0');                                           \
        int _sys = system(_cmd.data);                                    \
        buf_free(&_cmd);                                                  \
        unlink(out_path);                                                \
        free(spice_inc_dirs);                                            \
        return decode_exit_status(_sys);                                 \
    } while (0)

    /* Single-file mode: explicit file provided, skip project lookup. */
    if (explicit_file) {
        RUN_ENTRY(explicit_file);
    }

    /* Project mode: walk up to find build.tur. */
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "tur run: cannot get current directory\n");
        return 2;
    }

    char *root = find_project_root(cwd);
    if (!root) {
        fprintf(stderr,
            "tur run: no build.tur found (searched up from '%s')\n"
            "  Create a project with `tur new <name>`, "
            "or pass a file directly: tur run src/main.tur\n",
            cwd);
        return 1;
    }

    /* Parse manifest for entry point configuration. */
    char manifest_path[4096];
    snprintf(manifest_path, sizeof(manifest_path), "%s/build.tur", root);
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(manifest_path, &m)) { free(root); return 1; }

    /* Read lock file. */
    char lock_path[4096];
    snprintf(lock_path, sizeof(lock_path), "%s/tur.lock", root);
    PkgLockFile lock;
    memset(&lock, 0, sizeof(lock));
    lock.format_version = 1;
    pkg_lock_read(lock_path, &lock);

    /* Spice dependency handling. */
    if (m.n_spices > 0) {
        char spices_dir[4096];
        snprintf(spices_dir, sizeof(spices_dir), "%s/spices", root);

        if (offline) {
            /* Verify all required spices exist on disk. */
            bool missing = false;
            for (int i = 0; i < m.n_spices; i++) {
                const PkgSpice *s = &m.spices[i];
                if (s->path) continue; /* local path -- always present */
                char dep_dir[4096];
                if (s->ref)
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                             spices_dir, s->name, s->ref);
                else
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                             spices_dir, s->name);
                struct stat _dstat;
                if (stat(dep_dir, &_dstat) != 0 || !S_ISDIR(_dstat.st_mode)) {
                    fprintf(stderr,
                        "tur run: --offline: spice '%s' not found at '%s'\n",
                        s->name, dep_dir);
                    missing = true;
                }
            }
            if (missing) {
                pkg_lock_free(&lock);
                pkg_manifest_free(&m);
                free(root);
                return 1;
            }
        } else {
            /* Fetch any missing spices; verify SHA-256 of already-fetched ones. */
            bool need_fetch = false;
            for (int i = 0; i < m.n_spices; i++) {
                const PkgSpice *s = &m.spices[i];
                if (s->path) continue;
                char dep_dir[4096];
                if (s->ref)
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                             spices_dir, s->name, s->ref);
                else
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                             spices_dir, s->name);
                struct stat _dstat;
                if (stat(dep_dir, &_dstat) != 0 || !S_ISDIR(_dstat.st_mode)) {
                    need_fetch = true;
                } else {
                    /* Verify SHA-256 matches lock (if lock has entry). */
                    PkgLockEntry *le = pkg_lock_find(&lock, s->name, false);
                    if (le && le->sha256) {
                        char actual_sha[65];
                        if (pkg_sha256_dir(dep_dir, actual_sha) &&
                            strcmp(actual_sha, le->sha256) != 0) {
                            fprintf(stderr,
                                "tur run: integrity check failed for '%s'.\n"
                                "  Run `tur fetch --update` to re-download.\n",
                                s->name);
                            pkg_lock_free(&lock);
                            pkg_manifest_free(&m);
                            free(root);
                            return 1;
                        }
                    }
                }
            }
            if (need_fetch) {
                if (!pkg_fetch_all(root, &m, &lock, false)) {
                    fprintf(stderr, "tur run: dependency fetch failed\n");
                    pkg_lock_free(&lock);
                    pkg_manifest_free(&m);
                    free(root);
                    return 1;
                }
                pkg_lock_write(lock_path, &lock);
            }
        }

        /* Build spice include-path array.
         * Convention: spices/<name>-<ref>/src/ if it exists, else spices/<name>-<ref>/ */
        int inc_cap = m.n_spices;
        spice_inc_dirs = (const char **)malloc((size_t)inc_cap * sizeof(char *));
        n_spice_inc_dirs = 0;
        if (spice_inc_dirs) {
            for (int i = 0; i < m.n_spices; i++) {
                const PkgSpice *s = &m.spices[i];
                char dep_dir[4096];
                if (s->path) {
                    /* Local path dep: resolve relative to root */
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s", root, s->path);
                } else if (s->ref) {
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                             spices_dir, s->name, s->ref);
                } else {
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                             spices_dir, s->name);
                }
                /* Prefer dep_dir/src if it exists */
                char src_sub[4096];
                snprintf(src_sub, sizeof(src_sub), "%s/src", dep_dir);
                struct stat _ss;
                const char *chosen = (stat(src_sub, &_ss) == 0 && S_ISDIR(_ss.st_mode))
                                     ? src_sub : dep_dir;
                spice_inc_dirs[n_spice_inc_dirs++] = strdup(chosen);
            }
        }
    }

    /* CMake dependency handling: generate and build if cmake-deps present. */
    if (m.n_cmake_deps > 0) {
        /* Only (re)build if cmake/CMakeLists.txt doesn't already exist,
         * or if --update was requested.  For tur run we do a best-effort
         * build without --update so repeated runs stay fast. */
        char cmake_lists[4096];
        snprintf(cmake_lists, sizeof(cmake_lists), "%s/cmake/CMakeLists.txt", root);
        struct stat _cmst;
        bool cmake_built = (stat(cmake_lists, &_cmst) == 0);
        if (!cmake_built) {
            if (!pkg_gen_cmake_deps(root, &m) ||
                !pkg_cmake_build(root, &m, &lock)) {
                fprintf(stderr, "tur run: cmake dependency build failed\n");
                pkg_lock_free(&lock);
                pkg_manifest_free(&m);
                free(spice_inc_dirs);
                free(root);
                return 1;
            }
            pkg_lock_write(lock_path, &lock);
        }
    }

    pkg_lock_free(&lock);

    /* Entry point resolution (from the plan):
     *   1. :entry key in build.tur  (not yet in PkgManifest -- future)
     *   2. src/main.tur
     *   3. single .tur file in src/ */
    char entry[4096];
    struct stat _st;
    snprintf(entry, sizeof(entry), "%s/src/main.tur", root);
    if (stat(entry, &_st) != 0) {
        char src_dir[4096];
        snprintf(src_dir, sizeof(src_dir), "%s/src", root);
        int n_files = 0;
        char **files = collect_tur_files(src_dir, &n_files);
        if (n_files == 1) {
            strncpy(entry, files[0], sizeof(entry) - 1);
            entry[sizeof(entry) - 1] = '\0';
        } else {
            fprintf(stderr,
                "tur run: cannot determine entry point\n"
                "  Expected %s/src/main.tur, "
                "or exactly one .tur file in %s/src/\n",
                root, root);
            free_tur_files(files, n_files);
            pkg_manifest_free(&m);
            free(root);
            return 1;
        }
        free_tur_files(files, n_files);
    }

    pkg_manifest_free(&m);
    free(root);

    RUN_ENTRY(entry);
#undef RUN_ENTRY
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

        int build_rc = cmd_build(tur_files[i], out_path, NULL, 0);
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

    /* Collect cmake dep flags from cmake/spice-deps-manifest.json if present */
    Buf cmake_flags;
    buf_init(&cmake_flags);
    {
        char *proj_root = find_project_root(dir);
        if (proj_root) {
            char manifest_path[4096];
            snprintf(manifest_path, sizeof(manifest_path),
                     "%s/cmake/spice-deps-manifest.json", proj_root);
            PkgCmakeManifest cmake_manifest;
            if (pkg_cmake_manifest_read(manifest_path, &cmake_manifest)) {
                pkg_cmake_manifest_append_cc_flags(&cmake_manifest, &cmake_flags);
                pkg_cmake_manifest_free(&cmake_manifest);
            }
            free(proj_root);
        }
    }

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s %s -o %s", cc, cc_flags, out_path);
    /* Add _main.c first */
    buf_printf(&cmd, " _main.c");
    /* Add all .c files */
    for (int i = 0; i < n_files; i++) {
        buf_printf(&cmd, " %s", c_files[i]);
    }
    /* Append cmake dep flags (-I/-L/-l). */
    if (cmake_flags.len > 0) buf_puts(&cmd, cmake_flags.data);
    buf_free(&cmake_flags);
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

/* tur format [--check] [file]
 * Read source from file (or stdin if no file given), format it, and write to
 * stdout.  With --check, exit 1 if the file is not already formatted. */
static int cmd_format(const char *path, bool check_only) {
    char  *src = NULL;
    size_t len = 0;

    if (path) {
        if (read_entire_file(path, &src, &len) != 0) return 2;
    } else {
        /* Read from stdin */
        size_t cap = 4096;
        src = (char *)malloc(cap);
        if (!src) { fprintf(stderr, "tur: oom\n"); return 2; }
        len = 0;
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (len + 1 >= cap) {
                cap *= 2;
                char *tmp = (char *)realloc(src, cap);
                if (!tmp) { free(src); fprintf(stderr, "tur: oom\n"); return 2; }
                src = tmp;
            }
            src[len++] = (char)c;
        }
        src[len] = '\0';
    }

    SourceFile file = {0};
    file.path        = path ? path : "<stdin>";
    file.src         = src;
    file.len         = len;
    file.file_id     = 0;
    file.reader_type = READER_TURMERIC;
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
        FmtOptions opts = {0};
        opts.indent_width = 2;
        opts.line_width   = 80;
        opts.src          = src;
        opts.src_len      = len;

        Buf out;
        buf_init(&out);
        if (fmt_print(&out, forms, nforms, opts) != 0) {
            fprintf(stderr, "tur: fmt_print failed\n");
            rc = 1;
        } else if (check_only) {
            /* Exit 1 if already-formatted output differs from input */
            bool same = (out.len == len) && (memcmp(out.data, src, len) == 0);
            if (!same) {
                if (path) fprintf(stderr, "tur: %s is not formatted\n", path);
                rc = 1;
            }
        } else {
            buf_to_file(&out, stdout);
        }
        buf_free(&out);
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

/* Phase S0: tur repl — interactive read-eval-print loop. */
static int cmd_repl(void) {
    return turi_repl_run();
}

static int usage(void) {
    fprintf(stderr,
        "tur: the Turmeric compiler (phase 8)\n"
        "\n"
        "usage:\n"
        "  tur build <file.tur> [-o <out>]    build a single file\n"
        "  tur build <dir> [-o <out>]         build all .tur files in directory\n"
        "  tur emit-c <input.tur>            print the generated C to stdout\n"
        "  tur emit-h <input.tur>            print the generated header to stdout\n"
        "  tur run <input.tur>               build + execute a single file\n"
        "  tur repl                          interactive REPL (Phase S1)\n"
        "  tur test <dir>                    run all .tur files in a directory\n"
        "  tur check <input.tur>             type-check only, no codegen (phase 8)\n"
        "  tur format [--check] [file.tur]   format source (stdin if no file given)\n"
        "\n"
        "package management (Spice, Phase PKG-1):\n"
        "  tur init [--bin|--lib] <name>     create a new project\n"
        "  tur add <url> [--ref <tag>]       add a Turmeric spice\n"
        "  tur add <path> --path             add a local spice\n"
        "  tur add-cmake <url> [--ref <tag>] add a C/CMake dependency\n"
        "  tur fetch [--update]              download / update all spices\n"
        "  tur emit-cmake [--output-dir <d>] generate CMakeLists.txt + config for CMake consumers\n"
        "\n"
        "emit-c flags:\n"
        "  tur emit-c <file>                 compile a .tur file to C (stdout)\n"
        "  tur emit-c --output-dir <dir> <files...>  compile each .tur to <dir>/<mod>.h + .c\n"
        "\n"
        "global flags:\n"
        "  --no-color                       disable colored diagnostics\n"
        "  --json-diagnostics               output diagnostics as JSON (phase 8)\n"
        "  --explain <TUR-E####>            print explanation for a diagnostic code (HKT-P5)\n"
        "  --explain <snippet>              compile code snippet and explain errors (phase 8)\n"
        "  --dump-kinds                     dump kind annotations after kind-check (HKT-P6)\n"
        "  --backtrack-depth <N>            cap run-backtrack at N results (0=unlimited) (Phase B5)\n"
        "  --dump-clone-plan                dump cloneable capture plan after CPS (Phase B5)\n"
        "  --panic-abort                   all panics call abort() directly (Phase R5)\n"
        "  --panic-trace                   print scope chain on panic (Phase R6)\n"
        "  --warn-unused-result             warn on discarded result values (Phase R6)\n"
        "  --no-warn-unused-result          disable --warn-unused-result (Phase R6)\n"
        "  --lint-panic                     lint panic/must! usage (Phase R6)\n");
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

/* Phase R5: Handle --panic-abort flag */
static bool parse_panic_abort(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-abort") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase R6: Handle --panic-trace flag */
static bool parse_panic_trace(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-trace") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase R6: Handle --warn-unused-result flag */
static bool parse_warn_unused_result(int argc, char **argv) {
    bool enabled = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--warn-unused-result") == 0) {
            enabled = true;
        } else if (strcmp(argv[i], "--no-warn-unused-result") == 0) {
            enabled = false;
        }
    }
    return enabled;
}

/* Phase R6: Handle --lint-panic flag */
static bool parse_lint_panic(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lint-panic") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase 8: Handle --explain flag - compile code snippet and show detailed error */

/* Phase HKT-P5: Return true if `s` looks like a diagnostic code string
 * of the form "TUR-E" followed by one or more decimal digits. */
static bool looks_like_diag_code_(const char *s) {
    if (!s) return false;
    if (strncmp(s, "TUR-E", 5) != 0) return false;
    const char *p = s + 5;
    if (*p == '\0') return false;   /* need at least one digit */
    while (*p) {
        if (*p < '0' || *p > '9') return false;
        p++;
    }
    return true;
}

static int cmd_explain(const char *code) {
    /* Phase HKT-P5: If the argument looks like a diagnostic code (TUR-E####),
     * look it up in the explanation table instead of compiling a snippet. */
    if (looks_like_diag_code_(code)) {
        DiagCode dc = diag_code_from_string(code);
        if (dc == DIAG_CODE_NONE || !diag_explain(dc, stdout)) {
            fprintf(stderr, "tur: no explanation for '%s'\n", code);
            return 1;
        }
        return 0;
    }

    /* Legacy behaviour: compile a Turmeric source snippet and surface errors. */
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

    Expr *prog = elaborate_program(&arena, &st, forms, nforms, 0, ".", false, NULL,
                                    NULL, 0);
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
    g_panic_abort = parse_panic_abort(argc, argv);
    g_panic_trace = parse_panic_trace(argc, argv);
    g_warn_unused_result = parse_warn_unused_result(argc, argv);
    g_lint_panic = parse_lint_panic(argc, argv);
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-abort") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--panic-trace") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--warn-unused-result") == 0 ||
                   strcmp(argv[i], "--no-warn-unused-result") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--lint-panic") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--json-diagnostics") == 0) {
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
        } else if (strcmp(argv[i], "--lint-unsafe") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_warn_nested = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--lint-unsafe-max-lines=", 24) == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_max_lines = (uint32_t)atoi(argv[i] + 24);
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--require-unsafe-docs") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_require_safety = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--unsafe-stats") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_stats_enabled = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-kinds") == 0) {
            /* Phase HKT-P6: enable kind-annotation dump after kind-check */
            g_dump_kinds = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--backtrack-depth=", 18) == 0) {
            /* Phase B5: cap run-backtrack at N results (0 = unlimited) */
            g_backtrack_depth = (int64_t)atoll(argv[i] + 18);
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--backtrack-depth") == 0 && i + 1 < argc) {
            g_backtrack_depth = (int64_t)atoll(argv[i + 1]);
            for (int j = i; j < argc - 2; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        } else if (strcmp(argv[i], "--dump-clone-plan") == 0) {
            /* Phase B5: dump cloneable capture plan after CPS */
            g_dump_clone_plan = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "-Xgadt") == 0) {
            /* Phase G1: enable defgadt syntax */
            g_gadt_enabled = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
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
        /* tur emit-c --output-dir <dir> <file1> [<file2> ...] */
        if (argc >= 4 && strcmp(argv[2], "--output-dir") == 0) {
            const char *out_dir = argv[3];
            return cmd_emit_c_to_dir(out_dir, argv + 4, argc - 4);
        }
        /* tur emit-c <file> -- single file to stdout (legacy) */
        if (argc != 3) return usage();
        return cmd_emit_c(argv[2]);
    }
    if (strcmp(cmd, "emit-h") == 0) {
        if (argc != 3) return usage();
        return cmd_emit_h(argv[2]);
    }
    if (strcmp(cmd, "check") == 0) {
        /* Phase 8: tur check subcommand - type-check only, no codegen */
        if (argc != 3) return usage();
        Buf out;
        buf_init(&out);
        int rc = compile_to_c(argv[2], &out, NULL, 0);
        buf_free(&out);
        return rc;
    }
    if (strcmp(cmd, "build") == 0) {
        const char *input = NULL;
        const char *out = NULL;
        /* Collect -I flags from the command line */
        int     n_build_inc = 0;
        int     build_inc_cap = 4;
        char  **build_inc = (char **)malloc((size_t)build_inc_cap * sizeof(char *));
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
                if (n_build_inc >= build_inc_cap) {
                    build_inc_cap *= 2;
                    build_inc = (char **)realloc(build_inc,
                        (size_t)build_inc_cap * sizeof(char *));
                }
                build_inc[n_build_inc++] = argv[++i];
            } else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0') {
                if (n_build_inc >= build_inc_cap) {
                    build_inc_cap *= 2;
                    build_inc = (char **)realloc(build_inc,
                        (size_t)build_inc_cap * sizeof(char *));
                }
                build_inc[n_build_inc++] = argv[i] + 2;
            } else if (argv[i][0] != '-') {
                if (input) { free(build_inc); return usage(); }
                input = argv[i];
            } else {
                free(build_inc); return usage();
            }
        }
        if (!input) { free(build_inc); return usage(); }
        /* Check if input is a directory - use multi-file build */
        int rc;
        if (is_directory(input)) {
            rc = cmd_build_multi(input, out);
        } else {
            rc = cmd_build(input, out, (const char **)build_inc, n_build_inc);
        }
        free(build_inc);
        return rc;
    }
    if (strcmp(cmd, "run") == 0) {
        return cmd_run(argc, argv);
    }
    if (strcmp(cmd, "repl") == 0) {
        /* Phase S0: interactive REPL */
        return cmd_repl();
    }
    if (strcmp(cmd, "format") == 0) {
        bool check_only = false;
        const char *fmt_input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--check") == 0) {
                check_only = true;
            } else if (argv[i][0] != '-') {
                if (fmt_input) return usage();
                fmt_input = argv[i];
            } else {
                return usage();
            }
        }
        return cmd_format(fmt_input, check_only);
    }
    if (strcmp(cmd, "test") == 0) {
        if (argc != 3) return usage();
        return cmd_test(argv[2]);
    }
    /* Phase PKG-1: Spice package manager commands */
    if (strcmp(cmd, "new") == 0)
        return cmd_pkg_new(argc, argv);
    if (strcmp(cmd, "init") == 0)
        return cmd_pkg_init(argc, argv);
    if (strcmp(cmd, "add") == 0)
        return cmd_pkg_add(argc, argv);
    if (strcmp(cmd, "add-cmake") == 0)
        return cmd_pkg_add_cmake(argc, argv);
    if (strcmp(cmd, "fetch") == 0)
        return cmd_pkg_fetch(argc, argv);
    if (strcmp(cmd, "emit-cmake") == 0)
        return cmd_pkg_emit_cmake(argc, argv);
    return usage();
}
