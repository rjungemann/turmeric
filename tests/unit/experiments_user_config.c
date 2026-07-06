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
 * exit and turn the expected exit code 2 into a leak-abort.  Only declared
 * under ASan builds; on macOS Mach-O `__attribute__((weak))` on a plain
 * declaration does not produce a weak-undefined reference (that needs
 * `weak_import`), so Release links would fail with an undefined `___lsan_disable`.
 * Gate on the sanitizer feature macro instead. */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define TUR_HAVE_LSAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define TUR_HAVE_LSAN 1
#endif
#ifdef TUR_HAVE_LSAN
void __lsan_disable(void);
#endif

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

    /* The reader is exercised against the single surviving experiment,
     * `forall-dict-pass` (the four graduation-ready HKT/forall flags retired
     * 2026-07-06; see docs/archive/retire-graduation-ready-hkt-flags-plan.md).
     * When forall-dict-pass itself graduates, repoint these to whatever real
     * experiment then survives. */

    /* 1. Absent file -> no-op, returns false, nothing enabled. */
    remove_config(dir);
    CHECK(experiments_read_user_config() == false, "absent file returns false");
    CHECK(experiment_is_enabled("forall-dict-pass") == false,
          "absent file enables nothing");

    /* 2. Valid :enable list turns the named experiment on at user-config. */
    write_config(dir,
        ";; test config\n"
        ":enable [forall-dict-pass]\n");
    CHECK(experiments_read_user_config() == true, "present file returns true");
    CHECK(experiment_is_enabled("forall-dict-pass") == true,
          "forall-dict-pass enabled from user config");
    CHECK(experiment_is_enabled("no-such-experiment") == false,
          "an unlisted / unknown experiment stays off");

    /* Source column reports user-config for a flag that came from the file. */
    {
        bool saw_user_config = false;
        size_t n = experiment_count();
        for (size_t i = 0; i < n; i++) {
            const ExperimentDescriptor *d = experiment_at(i);
            if (strcmp(d->name, "forall-dict-pass") == 0) {
                saw_user_config =
                    (experiment_source_at(i) == XF_SRC_USER_CONFIG);
            }
        }
        CHECK(saw_user_config, "forall-dict-pass source is XF_SRC_USER_CONFIG");
    }

    /* 3. CLI beats user-config: a later CLI enable of the same flag wins. */
    CHECK(experiment_enable("forall-dict-pass", XF_SRC_CLI) == true,
          "CLI enable of already-user-config flag succeeds");
    {
        size_t n = experiment_count();
        for (size_t i = 0; i < n; i++) {
            const ExperimentDescriptor *d = experiment_at(i);
            if (strcmp(d->name, "forall-dict-pass") == 0) {
                CHECK(experiment_source_at(i) == XF_SRC_CLI,
                      "CLI overrides user-config source");
            }
        }
    }

    /* 4. Unknown key warns (TUR-W0062) but still returns true and applies the
     *    recognized :enable list. */
    write_config(dir,
        ":no-such-key [a b c]\n"
        ":enable [forall-dict-pass]\n");
    CHECK(experiments_read_user_config() == true,
          "unknown key does not abort the read");
    CHECK(experiment_is_enabled("forall-dict-pass") == true,
          "recognized :enable still applied after unknown key");

#ifndef _WIN32
    /* 5. Unknown experiment name exits with code 2 (TUR-E0310). Fork so the
     *    exit does not kill the test harness. */
    write_config(dir, ":enable [totally-bogus-experiment]\n");
    pid_t pid = fork();
    if (pid == 0) {
        /* child: exercise the exit(2) path.  Disable leak detection first --
         * the inherited heap is not ours to account for. */
#ifdef TUR_HAVE_LSAN
        __lsan_disable();
#endif
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
