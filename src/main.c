/* tur: the Turmeric compiler driver (phase 2). */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "arena.h"
#include "buf.h"
#include "borrow_check.h"  /* Phase 14 */
#include "diag.h"
#include "elab.h"
#include "emit.h"
#include "expr.h"
#include "forms.h"
#include "reader.h"
#include "symbols.h"

/* Extract basename from a path. */
static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
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

/* Reads a .tur file and emits its C source into `out_c`. Returns 0 on success,
 * nonzero on error (diagnostics already emitted). */
static int compile_to_c(const char *path, Buf *out_c) {
    char  *src = NULL;
    size_t len = 0;
    if (read_entire_file(path, &src, &len) != 0) return 2;

    SourceFile file = {0};
    file.path = path;
    file.src = src;
    file.len = len;
    file.file_id = 0;
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
        NULL
    };
    
    uint32_t total_stdlib_forms = 0;
    Form **all_stdlib_forms = NULL;
    uint8_t file_id = 1;
    
    /* Allocate SourceFile on arena to avoid stack-use-after-scope */
    SourceFile *stdlib_file = (SourceFile *)arena_alloc(&arena, sizeof(SourceFile));
    
    for (int i = 0; stdlib_files[i] != NULL; i++) {
        char *stdlib_src = NULL;
        size_t stdlib_len = 0;
        if (read_entire_file(stdlib_files[i], &stdlib_src, &stdlib_len) == 0) {
            /* strdup the source so it lives in the arena and won't be freed prematurely */
            char *src_copy = (char *)arena_alloc(&arena, stdlib_len);
            memcpy(src_copy, stdlib_src, stdlib_len);
            
            *stdlib_file = (SourceFile){0};
            stdlib_file->path = stdlib_files[i];
            stdlib_file->src = src_copy;
            stdlib_file->len = stdlib_len;
            stdlib_file->file_id = file_id++;
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
        Expr *prog = elaborate_program(&arena, &st, forms, nforms);
        if (!prog || diag_had_error()) {
            rc = 1;
        } else if (!borrow_check_program(prog)) {
            /* Phase 14: Borrow checker pass */
            rc = 1;
        } else if (emit_program(out_c, prog) != 0) {
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

    SourceFile file = {0};
    file.path = path;
    file.src = src;
    file.len = len;
    file.file_id = 0;
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
        Expr *prog = elaborate_program(&arena, &st, forms, nforms);
        if (!prog || diag_had_error()) {
            rc = 1;
        } else if (!borrow_check_program(prog)) {
            /* Phase 14: Borrow checker pass */
            rc = 1;
        } else if (emit_header(out_h, module_name, prog) != 0) {
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

    SourceFile file = {0};
    file.path = path;
    file.src = src;
    file.len = len;
    file.file_id = 0;
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
        Expr *prog = elaborate_program(&arena, &st, forms, nforms);
        if (!prog || diag_had_error()) {
            rc = 1;
        } else if (!borrow_check_program(prog)) {
            /* Phase 14: Borrow checker pass */
            rc = 1;
        } else if (emit_implementation(out_c, module_name, prog) != 0) {
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

static int cmd_build(const char *input, const char *out_path) {
    Buf csrc;
    buf_init(&csrc);
    int rc = compile_to_c(input, &csrc);
    if (rc != 0) { buf_free(&csrc); return rc; }

    /* Write C to a temp file. */
    char tmpl[] = "/tmp/tur-XXXXXX.c";
    int fd = mkstemps(tmpl, 2);
    if (fd < 0) {
        fprintf(stderr, "tur: cannot create temp file\n");
        buf_free(&csrc);
        return 2;
    }
    if (write(fd, csrc.data, csrc.len) != (ssize_t)csrc.len) {
        fprintf(stderr, "tur: write failed\n");
        close(fd);
        buf_free(&csrc);
        return 2;
    }
    close(fd);
    buf_free(&csrc);

    char chosen_out[1024];
    if (!out_path) {
        default_output_name(input, chosen_out, sizeof(chosen_out));
        out_path = chosen_out;
    }

    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s -O2 -std=c99 -Wall -o %s %s", cc, out_path, tmpl);
    int sys_rc = system(cmd.data);
    buf_free(&cmd);
    unlink(tmpl);

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

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s -O2 -std=c99 -Wall -o %s", cc, out_path);
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
    return usage();
}
