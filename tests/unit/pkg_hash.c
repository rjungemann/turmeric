/*
 * tests/unit/pkg_hash.c -- pkg_hash_dir, the spice lockfile's integrity hash.
 *
 * The properties here are the ones `tar -c <dir> | sha256sum` did not have,
 * each of which produced a real symptom:
 *
 *   path-independent   `tar -c /abs/path` puts the absolute path in the member
 *                      headers, so the digest depended on where the project
 *                      lived on disk.
 *   .git-insensitive   a shallow clone's .git/index records mtimes, so running
 *                      any git command inside a fetched spice changed its hash
 *                      and `tur run` reported an integrity failure for a tree
 *                      nobody had touched.
 *   unambiguous        a path and the bytes that follow it must not be able to
 *                      slide into each other.
 *
 * plus the tagging that keeps a lockfile an older tur wrote from being read as
 * tampering.  See docs/reported/pkg-hash-shells-out-to-sha256sum.md.
 */

#include "compiler/pkg.h"
#include "lsp/lsp_sym.h"            /* LspSymbol, for the link stub below */
#include "platform_fs.h"

#include <dirent.h>   /* opendir/readdir -- rm_tree */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>   /* rmdir -- an implicit declaration is a hard error
                      * under clang 16+, which is how this passed on Windows
                      * (platform_fs.h supplies both) and failed on macOS. */

/* Stub: tur_core's lsp.c references this; this test never touches LSP, but the
 * symbol must resolve when tur_core is linked into a standalone executable.
 * Matches the stub in tests/unit/refine_solver.c. */
int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    (void)source_path;
    (void)logical_path;
    (void)out;
    (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

static int failures = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }            \
    else         { printf("ok:   %s\n", (msg)); }                        \
} while (0)

#define ROOT "pkg_hash_unit_tmp"

static void rm_tree(const char *path);

static void write_file(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot write %s\n", path); failures++; return; }
    fwrite(body, 1, strlen(body), f);
    fclose(f);
}

static void make_dir(const char *path) {
    mkdir(path, 0755);
}

/* Builds the tree the tests compare against:
 *     <root>/build.tur          ":name demo"
 *     <root>/src/demo.tur       "(defn main [] : int 0)"
 *     <root>/src/nested/x.txt   "x"
 */
static void make_tree(const char *root) {
    char p[512];
    make_dir(root);
    snprintf(p, sizeof p, "%s/build.tur", root);
    write_file(p, ":name demo");
    snprintf(p, sizeof p, "%s/src", root);
    make_dir(p);
    snprintf(p, sizeof p, "%s/src/demo.tur", root);
    write_file(p, "(defn main [] : int 0)");
    snprintf(p, sizeof p, "%s/src/nested", root);
    make_dir(p);
    snprintf(p, sizeof p, "%s/src/nested/x.txt", root);
    write_file(p, "x");
}

int main(void) {
    rm_tree(ROOT);
    make_dir(ROOT);

    char a[512], b[512], p[512];
    snprintf(a, sizeof a, "%s/a", ROOT);
    snprintf(b, sizeof b, "%s/deeper/path/b", ROOT);

    make_tree(a);
    snprintf(p, sizeof p, "%s/deeper", ROOT);            make_dir(p);
    snprintf(p, sizeof p, "%s/deeper/path", ROOT);       make_dir(p);
    make_tree(b);

    char h1[PKG_HASH_MAX], h2[PKG_HASH_MAX], hb[PKG_HASH_MAX];

    CHECK(pkg_hash_dir(a, h1), "pkg_hash_dir succeeds on a real tree");
    CHECK(strncmp(h1, PKG_TREE_HASH_TAG, sizeof(PKG_TREE_HASH_TAG) - 1) == 0,
          "the hash carries its algorithm tag");
    CHECK(strlen(h1) == sizeof(PKG_TREE_HASH_TAG) - 1 + 64,
          "tag plus 64 hex chars");

    CHECK(pkg_hash_dir(a, h2) && strcmp(h1, h2) == 0,
          "same tree hashes the same twice");

    CHECK(pkg_hash_dir(b, hb) && strcmp(h1, hb) == 0,
          "same content at a different path hashes the same");

    /* .git is metadata, not content. */
    snprintf(p, sizeof p, "%s/.git", a);                 make_dir(p);
    snprintf(p, sizeof p, "%s/.git/index", a);           write_file(p, "mtimes and pack junk");
    CHECK(pkg_hash_dir(a, h2) && strcmp(h1, h2) == 0,
          ".git contents do not affect the hash");

    /* Build outputs appear only after someone builds in the tree. */
    snprintf(p, sizeof p, "%s/build", a);                make_dir(p);
    snprintf(p, sizeof p, "%s/build/demo.o", a);         write_file(p, "object code");
    CHECK(pkg_hash_dir(a, h2) && strcmp(h1, h2) == 0,
          "build/ does not affect the hash");

    /* Content changes must be visible -- this is the whole point. */
    snprintf(p, sizeof p, "%s/src/demo.tur", a);
    write_file(p, "(defn main [] : int 1)");
    CHECK(pkg_hash_dir(a, h2) && strcmp(h1, h2) != 0,
          "a one-byte content change changes the hash");
    write_file(p, "(defn main [] : int 0)");
    CHECK(pkg_hash_dir(a, h2) && strcmp(h1, h2) == 0,
          "restoring the content restores the hash");

    /* So must a rename, even with the bytes unchanged. */
    {
        char from[512], to[512];
        snprintf(from, sizeof from, "%s/src/nested/x.txt", a);
        snprintf(to,   sizeof to,   "%s/src/nested/y.txt", a);
        CHECK(rename(from, to) == 0, "renamed a file");
        CHECK(pkg_hash_dir(a, h2) && strcmp(h1, h2) != 0,
              "a rename changes the hash");
        rename(to, from);
    }

    /* A path and the bytes after it must not slide into one another:
     * {"ab" -> "c"} and {"a" -> "bc"} are different trees. */
    {
        char t1[512], t2[512], g1[PKG_HASH_MAX], g2[PKG_HASH_MAX];
        snprintf(t1, sizeof t1, "%s/amb1", ROOT);
        snprintf(t2, sizeof t2, "%s/amb2", ROOT);
        make_dir(t1);
        make_dir(t2);
        snprintf(p, sizeof p, "%s/ab", t1); write_file(p, "c");
        snprintf(p, sizeof p, "%s/a", t2);  write_file(p, "bc");
        CHECK(pkg_hash_dir(t1, g1) && pkg_hash_dir(t2, g2) && strcmp(g1, g2) != 0,
              "path and content cannot slide into each other");
    }

    CHECK(!pkg_hash_dir(ROOT "/no-such-directory", h2),
          "a missing directory fails rather than hashing nothing");

    /* Only this algorithm's own output is comparable.  Everything an older tur
     * could have written must be skipped, not reported as tampering. */
    CHECK(pkg_hash_comparable(h1), "a tagged hash is comparable");
    CHECK(!pkg_hash_comparable(
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
          "a legacy bare tar|sha256sum digest is not comparable");
    CHECK(!pkg_hash_comparable("0123456789abcdef0123456789abcdef01234567"),
          "the git-SHA fallback is not comparable");
    CHECK(!pkg_hash_comparable(NULL), "a missing hash is not comparable");

    rm_tree(ROOT);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall pkg_hash checks passed\n");
    return 0;
}

/* Depth-first unlink; only ever called on ROOT, which this test created. */
static void rm_tree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char child[1024];
            snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
            struct stat st;
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) rm_tree(child);
            else remove(child);
        }
        closedir(d);
    }
    rmdir(path);
}
