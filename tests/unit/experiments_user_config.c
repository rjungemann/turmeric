/* experiments_user_config.c -- unit test for experiments_read_user_config
 * (UC-2, user-config-experiments-plan).
 *
 * Drives the reader against a temp $XDG_CONFIG_HOME so no real user file is
 * touched.  Covers: absent file -> no-op false; a valid :enable list turns
 * the named experiments on at XF_SRC_USER_CONFIG; an unknown key warns
 * (TUR-W0062) but still succeeds; and (via fork) an unknown experiment name
 * exits the process with code 2 (TUR-E0310).
 *
 * Built as tur_experiments_user_config; registered with ctest in the root
 * CMakeLists.txt. */
#include "experiments.h"
#include "lsp_sym.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Stub: tur_core's lsp.c references this; this test doesn't touch LSP, but the
 * symbol must resolve when we link tur_core into a standalone executable.
 * Matches the stub in tests/runtime/ffi-dispatch-unit.c. */
int tur_collect_symbols(const char *source_path, LspSymbol *out, int cap,
                        int *count_out) {
    (void)source_path;
    (void)out;
    (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

/* Suppress LeakSanitizer in the forked child below: it inherits the parent's
 * still-live allocations, which LSan would otherwise report at the child's
 * exit and turn the expected exit code 2 into a leak-abort.  Weakly declared
 * so Release (non-ASan) builds link without the sanitizer runtime. */
void __lsan_disable(void) __attribute__((weak));

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++;                                                    \
        } else {                                                           \
            fprintf(stderr, "ok: %s\n", (msg));                            \
        }                                                                  \
    } while (0)

/* Point XDG_CONFIG_HOME at `dir` and create dir/turmeric/. */
static void set_config_home(const char *dir) {
    setenv("XDG_CONFIG_HOME", dir, 1);
    /* Ensure HOME does not accidentally shadow a real file if XDG were
     * cleared; keeping XDG set is what the reader prefers. */
    char sub[4096];
    snprintf(sub, sizeof(sub), "%s/turmeric", dir);
    mkdir(dir, 0755);
    mkdir(sub, 0755);
}

/* Write `contents` to $XDG_CONFIG_HOME/turmeric/experiments.tur. */
static void write_config(const char *dir, const char *contents) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/turmeric/experiments.tur", dir);
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(3); }
    fputs(contents, f);
    fclose(f);
}

static void remove_config(const char *dir) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/turmeric/experiments.tur", dir);
    remove(path);
}

int main(void) {
    char tmpl[] = "/tmp/tur-uc-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 3; }
    set_config_home(dir);

    /* 1. Absent file -> no-op, returns false, nothing enabled. */
    remove_config(dir);
    CHECK(experiments_read_user_config() == false, "absent file returns false");
    CHECK(experiment_is_enabled("forall-kinds") == false,
          "absent file enables nothing");

    /* 2. Valid :enable list turns the named experiments on at user-config. */
    write_config(dir,
        ";; test config\n"
        ":enable [forall-kinds\n"
        "         hkt-hrt]\n");
    CHECK(experiments_read_user_config() == true, "present file returns true");
    CHECK(experiment_is_enabled("forall-kinds") == true,
          "forall-kinds enabled from user config");
    CHECK(experiment_is_enabled("hkt-hrt") == true,
          "hkt-hrt enabled from user config");
    CHECK(experiment_is_enabled("vl-wide-functor") == false,
          "unlisted experiment stays off");

    /* Source column reports user-config for a flag that came from the file. */
    {
        bool saw_user_config = false;
        size_t n = experiment_count();
        for (size_t i = 0; i < n; i++) {
            const ExperimentDescriptor *d = experiment_at(i);
            if (strcmp(d->name, "forall-kinds") == 0) {
                saw_user_config =
                    (experiment_source_at(i) == XF_SRC_USER_CONFIG);
            }
        }
        CHECK(saw_user_config, "forall-kinds source is XF_SRC_USER_CONFIG");
    }

    /* 3. CLI beats user-config: a later CLI enable of the same flag wins. */
    CHECK(experiment_enable("forall-kinds", XF_SRC_CLI) == true,
          "CLI enable of already-user-config flag succeeds");
    {
        size_t n = experiment_count();
        for (size_t i = 0; i < n; i++) {
            const ExperimentDescriptor *d = experiment_at(i);
            if (strcmp(d->name, "forall-kinds") == 0) {
                CHECK(experiment_source_at(i) == XF_SRC_CLI,
                      "CLI overrides user-config source");
            }
        }
    }

    /* 4. Unknown key warns (TUR-W0062) but still returns true and enables the
     *    recognized :enable list. */
    write_config(dir,
        ":no-such-key [a b c]\n"
        ":enable [forall-constraints]\n");
    CHECK(experiments_read_user_config() == true,
          "unknown key does not abort the read");
    CHECK(experiment_is_enabled("forall-constraints") == true,
          "recognized :enable still applied after unknown key");

#ifndef _WIN32
    /* 5. Unknown experiment name exits with code 2 (TUR-E0310). Fork so the
     *    exit does not kill the test harness. */
    write_config(dir, ":enable [totally-bogus-experiment]\n");
    pid_t pid = fork();
    if (pid == 0) {
        /* child: exercise the exit(2) path.  Disable leak detection first --
         * the inherited heap is not ours to account for. */
        if (__lsan_disable) __lsan_disable();
        experiments_read_user_config();
        _exit(0);   /* should not reach here */
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2,
              "unknown experiment name exits with code 2");
    } else {
        perror("fork");
        failures++;
    }
#endif

    remove_config(dir);
    {
        char sub[4096];
        snprintf(sub, sizeof(sub), "%s/turmeric", dir);
        rmdir(sub);
        rmdir(dir);
    }

    if (failures) {
        fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "\nall checks passed\n");
    return 0;
}
