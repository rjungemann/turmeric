/* fuzz_manifest -- the spice manifest reader, pkg_manifest_read (T1).
 *
 * build.tur is the first file `tur check`, the LSP and `tur build` read in a
 * tree, including every transitive spice's.  The reader takes a path, so each
 * input is written to a private scratch file; the first byte picks the plain
 * (build.tur) or sweet-exp (build.tur.sweet) spelling. */
#include "fuzz_common.h"
#include "compiler/pkg.h"
#include "compiler/diag.h"

#include <stdio.h>
#include <unistd.h>

static char g_dir[256];

static void drop_diag(DiagLevel level, const char *code, const char *file,
                      uint32_t line, uint32_t col_start, uint32_t col_end,
                      const char *message, void *ud) {
    (void)level; (void)code; (void)file; (void)line;
    (void)col_start; (void)col_end; (void)message; (void)ud;
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    setenv("TUR_DEBUG_ARENA_POISON", "0", 1);
    diag_set_sink(drop_diag, NULL);
    const char *t = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/tur-fuzz-manifest-XXXXXX", (t && *t) ? t : "/tmp");
    if (!mkdtemp(g_dir)) abort();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    char path[320];
    snprintf(path, sizeof path, "%s/%s", g_dir,
             (data[0] & 1) ? "build.tur.sweet" : "build.tur");
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    if (size > 1) fwrite(data + 1, 1, size - 1, f);
    fclose(f);

    diag_reset();
    pkg_manifest_malformed_reset();
    pkg_tur_version_reset();
    PkgManifest m;
    memset(&m, 0, sizeof m);
    if (pkg_manifest_read(path, &m)) pkg_manifest_free(&m);
    diag_reset();
    unlink(path);
    return 0;
}
