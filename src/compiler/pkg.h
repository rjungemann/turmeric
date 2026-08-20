#ifndef TUR_PKG_H
#define TUR_PKG_H

/*
 * pkg.h -- Spice: the Turmeric package manager (Phase PKG-1)
 *
 * A build.tur file declares a package using (defpackage ...).
 * tur.lock pins all resolved git refs and SHA-256 hashes.
 *
 * Workflow:
 *   tur init --bin my-app    create a new binary project
 *   tur init --lib my-lib    create a new library project
 *   tur add <url> [--ref v]  add a Turmeric spice (git URL)
 *   tur add <path> --path    add a local spice
 *   tur add-cmake <url>      add a C/CMake dependency
 *   tur fetch                clone all spices, update tur.lock
 *   tur fetch --update       update all spices to latest ref
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "buf.h"

/* ------------------------------------------------------------------ */
/* CMake option key/value pair                                          */
/* ------------------------------------------------------------------ */

typedef struct PkgCmakeOpt {
    char *key;
    char *val;
} PkgCmakeOpt;

/* ------------------------------------------------------------------ */
/* A single cmake dependency entry from :cmake-deps                    */
/* ------------------------------------------------------------------ */

typedef struct PkgCmakeDep {
    char         *name;
    char         *url;         /* git URL; NULL for local-path deps */
    char         *ref;         /* git tag/branch/SHA; NULL for path deps */
    char         *path;        /* local path; NULL for git deps */
    char         *cmake_name;  /* find_package name if different from key */
    bool          prefer_system;   /* try find_package before FetchContent */
    char         *cmake_version;   /* optional minimum version for find_package */
    char        **targets;     /* CMake targets to link against */
    int           n_targets;
    PkgCmakeOpt  *opts;
    int           n_opts;
} PkgCmakeDep;

/* ------------------------------------------------------------------ */
/* Parsed spice-deps-manifest.json entry                               */
/* ------------------------------------------------------------------ */

typedef struct PkgCmakeManifestEntry {
    char  *name;
    char  *resolved_via;   /* "system", "fetch", "path", or NULL if absent */
    char  *system_version; /* find_package version when resolved_via==system */
    char **include_dirs;
    int    n_include_dirs;
    char **link_dirs;
    int    n_link_dirs;
    char **link_libs;
    int    n_link_libs;
} PkgCmakeManifestEntry;

typedef struct PkgCmakeManifest {
    PkgCmakeManifestEntry *entries;
    int                    n_entries;
} PkgCmakeManifest;

/* ------------------------------------------------------------------ */
/* A single Turmeric spice (package) dependency                        */
/* ------------------------------------------------------------------ */

typedef struct PkgSpice {
    char *name;
    char *url;      /* NULL for local-path deps */
    char *ref;      /* git tag/branch/SHA; NULL for local-path deps */
    char *path;     /* relative local path; NULL for git deps */
    char *subdir;   /* subdirectory within repo (monorepo sub-packages); NULL = root */
    bool  optional;
} PkgSpice;

/* ------------------------------------------------------------------ */
/* The parsed content of a build.tur file                              */
/* ------------------------------------------------------------------ */

typedef struct PkgManifest {
    char        *name;
    char        *version;
    /* `:tur-version "<range>"` -- which COMPILER versions this spice is valid
     * under, as opposed to `version` above, which is the spice's own version.
     * NULL when unconstrained (the overwhelming majority today).  See
     * docs/archive/history/no-compiler-version-constraint-in-manifest.md. */
    char        *tur_version;
    char        *description;
    char        *license;
    char       **authors;
    int          n_authors;
    char        *repository;
    char        *homepage;
    PkgSpice    *spices;
    int          n_spices;
    PkgCmakeDep *cmake_deps;
    int          n_cmake_deps;
    /* source files exported as a C library (used by tur emit-cmake) */
    char       **exports;
    int          n_exports;
    /* build options */
    char       **c_flags;
    int          n_c_flags;
    char       **link_libs;
    int          n_link_libs;
    /* spices-c-sources-plan: auxiliary hand-written C sources vendored into
     * the spice (e.g. KissFFT, stb_image). Paths are stored as written in
     * build.tur (relative to the manifest dir) and compiled + linked into the
     * consuming binary. c_includes are -I dirs (also manifest-relative) made
     * visible to both the aux C compile and the spice's own inline-C. */
    char       **c_sources;
    int          n_c_sources;
    char       **c_includes;
    int          n_c_includes;
    bool         no_stdlib;
    /* RM4: reader-macro files loaded implicitly for every source file in
     * this spice. Paths are stored as written in build.tur (relative to
     * the manifest directory unless absolute). */
    char       **reader_macros;
    int          n_reader_macros;
    /* GS-M1: declared binaries. Parallel arrays: bin_names[i] is the
     * binary name (must start with "tur-"); bin_paths[i] is the
     * entrypoint module path relative to the manifest dir. */
    char       **bin_names;
    char       **bin_paths;
    int          n_bins;
    /* LS2 (local-spice-dev-workflow): workspace members. A non-empty
     * :members [...] in a manifest declares this directory as a
     * workspace root, listing the paths (relative to this manifest)
     * of member spices. Used by the resolver so sibling members can
     * import each other without an explicit :spices entry. */
    char       **members;
    int          n_members;
    /* build-output-directory-plan: relative path (from the manifest dir) for
     * generated artifacts. NULL = use the default (`<manifest-dir>/build`). */
    char        *build_dir;
    /* `:entry "src/foo.tur"` -- the project's entry-point module for
     * `tur run` in project mode, relative to the manifest dir (an absolute
     * path is honored as written). NULL when the key is absent, in which
     * case resolution falls back to src/main.tur and then to the single
     * .tur file under src/. */
    char        *entry;
    /* engine-selection-plan E1: default execution engine for `tur run` --
     * "cc" | "jit" | "interp", or NULL when the key is absent.  Validated at
     * parse (TUR-E0311); resolved by main.c's resolve_engine ladder
     * (CLI --engine > TUR_ENGINE env > this key > "cc"). */
    char        *engine;
    /* XF1 (experimental-flag-mechanism-plan): names from a top-level
     * :experiments [...] list. Each is a kebab-case experiment name (stored
     * without any leading ':'); merged with the CLI --enable= set at build
     * time (CLI wins on conflict). An unknown name is a hard TUR-E0310 error. */
    char       **experiments;
    int          n_experiments;
    /* UC-3 (user-config-experiments-plan): true iff the manifest carried an
     * :experiments key at all -- even the empty list `:experiments []`.
     * Distinguishes "no key" from "empty key"; the latter still suppresses
     * the user-level ~/.config/turmeric/experiments.tur file (Goal 2). */
    bool         has_experiments_key;
} PkgManifest;

/* ------------------------------------------------------------------ */
/* A single entry in tur.lock                                          */
/* ------------------------------------------------------------------ */

typedef struct PkgLockEntry {
    char  *name;
    char  *url;
    char  *ref;
    char  *resolved;    /* full git commit SHA */
    char  *sha256;      /* SHA-256 of the archive */
    char  *fetched_at;  /* ISO-8601 timestamp */
    char  *resolved_via;   /* "system" or "fetch"; NULL = legacy/fetch */
    char  *system_version; /* system pkg version when resolved_via == system */
    char **transitive;  /* "name@ref" strings */
    int    n_transitive;
    bool   is_cmake;    /* true = lives under cmake-deps: section */
} PkgLockEntry;

/* ------------------------------------------------------------------ */
/* The parsed content of a tur.lock file                               */
/* ------------------------------------------------------------------ */

typedef struct PkgLockFile {
    int           format_version;
    PkgLockEntry *entries;
    int           n_entries;
} PkgLockFile;

/* ------------------------------------------------------------------ */
/* Manifest read / write                                               */
/* ------------------------------------------------------------------ */

/* The outcome of a manifest read.  ABSENT and MALFORMED are very different
 * situations that a plain `false` used to collapse into one:
 *
 *   ABSENT     -- no manifest here (or it could not be opened).  Normal; the
 *                 caller should carry on with whatever resolution it had.
 *   OK         -- read and parsed.
 *   MALFORMED  -- a manifest EXISTS and is broken.  The caller is about to
 *                 silently drop everything the manifest was going to provide
 *                 (most damagingly, the spice root's `src/` never joins the
 *                 module search path), so every intra-spice import then fails
 *                 with `module not found` naming the import, not the manifest.
 *
 * See docs/archive/manifest-read-failure-degrades-to-module-not-found.md. */
typedef enum {
    PKG_MANIFEST_ABSENT = 0,
    PKG_MANIFEST_OK,
    PKG_MANIFEST_MALFORMED,
} PkgManifestStatus;

/* Parse a build.tur file.  All returned strings are heap-allocated.
 * Returns true on success; prints diagnostics to stderr on failure.
 *
 * On failure `*out` is freed and zeroed before returning, so a caller taking
 * the `if (!pkg_manifest_read(...)) continue;` branch leaks nothing and never
 * sees a partially-parsed manifest. */
bool pkg_manifest_read(const char *path, PkgManifest *out);

/* Same, reporting WHICH failure occurred.  `status` may be NULL. */
bool pkg_manifest_read_status(const char *path, PkgManifest *out,
                              PkgManifestStatus *status);

/* True if any manifest read in this process found a manifest and rejected it.
 * Sticky across diag_reset() -- see pkg_manifest_reassert().  When true,
 * `pkg_manifest_malformed_path()` is the offending manifest (NULL if none). */
bool        pkg_manifest_malformed(void);
const char *pkg_manifest_malformed_path(void);

/* Re-assert a malformed-manifest verdict after diag_reset(), so the command
 * actually FAILS rather than printing `error:` and exiting 0.  Same shape and
 * same reason as pkg_tur_version_reassert(); call the two together. */
void pkg_manifest_reassert(void);
void pkg_manifest_malformed_reset(void);

/* Resolve a manifest filename in `dir`. Writes the full path into `out`
 * (size `cap`). Probes `build.tur` first, then `build.tur.sweet`.
 * Returns true if either exists as a regular file. */
bool pkg_resolve_manifest_path(const char *dir, char *out, size_t cap);

/* Same, but the path is cwd-relative ("build.tur" or "build.tur.sweet"). */
bool pkg_resolve_manifest_cwd(char *out, size_t cap);

/* Returns true if `name` is one of the recognised manifest filenames
 * ("build.tur" or "build.tur.sweet"). */
bool pkg_is_manifest_name(const char *name);

/* Write a build.tur file from a PkgManifest struct. */
bool pkg_manifest_write(const char *path, const PkgManifest *m);

void pkg_manifest_free(PkgManifest *m);

/* ------------------------------------------------------------------ */
/* Lock file read / write                                              */
/* ------------------------------------------------------------------ */

/* Parse a tur.lock file.  Returns true on success. */
bool pkg_lock_read(const char *path, PkgLockFile *out);

/* Serialise a PkgLockFile to tur.lock (Turmeric S-expression).  Returns true on success. */
bool pkg_lock_write(const char *path, const PkgLockFile *lock);

void pkg_lock_free(PkgLockFile *lock);

/* Find an entry in the lock file by name (and cmake flag). */
PkgLockEntry *pkg_lock_find(PkgLockFile *lock, const char *name, bool is_cmake);

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

/* Compute SHA-256 of a file (64 hex chars + NUL). Returns true on success. */
bool pkg_sha256_file(const char *path, char out[65]);

/* Compute SHA-256 of a directory by piping `tar -c <dir>` into sha256.
 * Returns true on success; out receives 64 hex chars + NUL. */
bool pkg_sha256_dir(const char *dir, char out[65]);

/* Parse semver "vMAJOR.MINOR.PATCH[-pre]" or "MAJOR.MINOR.PATCH[-pre]".
 * Returns false if the string is not valid semver. */
bool pkg_semver_parse(const char *v,
                      int *major, int *minor, int *patch,
                      char **pre);

/* Compare two semver strings.  Returns <0, 0, or >0 like strcmp.  A
 * pre-release ranks BELOW the same version without one (1.0.0-rc1 < 1.0.0). */
int pkg_semver_compare(const char *a, const char *b);

/* Version ranges, for `:tur-version` (see the grammar comment in pkg.c).
 *
 *   pkg_version_range_valid  -- is `range` syntactically a range at all?
 *                               Call this before match(); match() treats an
 *                               unparseable range as vacuously satisfied so a
 *                               malformed constraint cannot silently reject.
 *   pkg_version_range_match  -- does `version` satisfy every conjunct?
 *                               `out_below_floor` (optional) reports that the
 *                               failure was a LOWER bound, which callers treat
 *                               as a hard error; an upper-bound-only failure
 *                               means "untested against", i.e. a warning. */
bool pkg_version_range_valid(const char *range);
bool pkg_version_range_match(const char *range, const char *version,
                             bool *out_below_floor);

/* `:tur-version` is diagnosed inside pkg_manifest_read (TUR-E0622 malformed
 * range, TUR-E0621 below floor -- hard error, TUR-W0623 above ceiling --
 * warning), so every manifest-discovering entry point gets the check for free.
 * Reported at most once per process; silent when the manifest declares no range
 * or the build cannot report its own version.
 *
 * A rejecting verdict must survive diag_reset(), which every
 * compile entry point calls to keep batch drivers from poisoning later files.
 * Without this the floor error printed and then exited 0 -- an "error" that did
 * not fail.  Call reassert() right after diag_reset() (beside
 * experiment_reset_warnings()); it re-emits a brief form so THIS compile fails,
 * and is a no-op when the manifest declared no range or the range was met.
 * reset() clears the sticky state for in-process drivers that switch projects. */
void pkg_tur_version_reassert(void);
void pkg_tur_version_reset(void);

/* ------------------------------------------------------------------ */
/* Git / fetch operations                                              */
/* ------------------------------------------------------------------ */

/* Clone or update a git repo into dest_dir at the given ref.
 * Returns the resolved commit SHA (heap-allocated) on success, NULL on error.
 * Caller must free() the returned string. */
char *pkg_git_fetch(const char *url, const char *ref, const char *dest_dir);

/* Resolve current HEAD SHA in an already-cloned directory.
 * Returns heap-allocated SHA string; caller must free(). */
char *pkg_git_resolve(const char *repo_dir);

/* ------------------------------------------------------------------ */
/* High-level package operations                                       */
/* ------------------------------------------------------------------ */

/* Fetch all spices (and transitively their deps) declared in manifest
 * into <project_dir>/spices/.  Updates lock in place.
 * update=true allows upgrading refs beyond what is already in the lock.
 * Returns true on success. */
bool pkg_fetch_all(const char *project_dir,
                   const PkgManifest *manifest,
                   PkgLockFile *lock,
                   bool update);

/* Generate cmake/CMakeLists.txt from the :cmake-deps block.
 * Returns true on success. */
bool pkg_gen_cmake_deps(const char *project_dir,
                        const PkgManifest *manifest);

/* Invoke cmake to configure and build the cmake/ subproject.
 * Updates lock entries with resolved git SHAs.
 * Returns true on success. */
/* target: NULL for native, "wasm" to use emcmake/Emscripten toolchain. */
bool pkg_cmake_build(const char *project_dir,
                     const PkgManifest *manifest,
                     PkgLockFile *lock,
                     const char *target);

/* LS4: returns true iff `project_dir` is a member of an enclosing
 * workspace (build.tur with `:members`) AND that workspace lists a
 * sibling member named `name` (matched against each member's own
 * `:name`, falling back to the basename of the member path).  Used at
 * resolution time so workspace-sibling `:spices` entries get the same
 * "skip URL fetch, no lockfile row required" treatment as `:path`
 * deps. */
bool pkg_is_workspace_member(const char *project_dir, const char *name);

/* LS4: resolve a workspace-sibling `:spices` entry to the absolute path
 * of the sibling's directory.  Returns NULL if no enclosing workspace
 * lists a matching sibling member.  Caller frees. */
char *pkg_workspace_member_path(const char *project_dir, const char *dep_name);

/* Walk `root_manifest`'s :spices block transitively, resolving each spice
 * to its on-disk directory (workspace-sibling preferred, then :path, then
 * fetched <root>/spices/<name>[-<ref>][/<subdir>]), reading the sibling's
 * own build.tur, and unioning all :cmake-deps blocks encountered into a
 * freshly-allocated PkgCmakeDep array.  The root manifest's own cmake_deps
 * are included first so its declarations win in any ordering-sensitive
 * downstream consumer.
 *
 * Conflict policy:
 *   - identical (name, url, ref) entries are silently deduplicated
 *     (workspace siblings sharing a system dep coexist cleanly);
 *   - same-name entries with mismatched :url or :ref are a hard error --
 *     stderr gets both origins and the function returns false.
 *
 * On success, *out_deps points to a newly-allocated array of PkgCmakeDep
 * (deep-copied from source manifests; :path entries are absolutized so
 * the existing CMake generator's `%s/%s` join with the root project_dir
 * does not break).  *out_n is the count.  Caller must free with
 * pkg_cmake_deps_free.  Returns true on success (including the no-deps
 * case where *out_n stays 0). */
/* `include_workspace_siblings`: when true, the walk also seeds the worklist
 * with every workspace sibling member (matching the historical "any sibling
 * is implicitly importable" rule used by `tur run`).  When false, only the
 * manifest's own `:spices` closure contributes -- the narrower semantic
 * `tur build .` wants so it doesn't accidentally try to configure cmake-deps
 * from unrelated workspace members.  See
 * docs/archive/history/tur-build-cmake-deps-workspace-overreach.md. */
bool pkg_collect_transitive_cmake_deps(const char        *root_project_dir,
                                       const PkgManifest *root_manifest,
                                       bool               include_workspace_siblings,
                                       PkgCmakeDep      **out_deps,
                                       int               *out_n);

/* Free an array allocated by pkg_collect_transitive_cmake_deps. */
void pkg_cmake_deps_free(PkgCmakeDep *deps, int n);

/* Verify that each cmake dep's resolved SHA still matches tur.lock.
 * Returns true if all match (or lock has no entry yet).
 * Prints a diagnostic and returns false if a mismatch is found. */
bool pkg_cmake_verify_lock(const char *project_dir,
                            const PkgLockFile *lock);

/* Parse cmake/spice-deps-manifest.json.
 * Returns true on success (file not present is not an error -- returns true
 * with n_entries == 0). */
bool pkg_cmake_manifest_read(const char *path, PkgCmakeManifest *out);

void pkg_cmake_manifest_free(PkgCmakeManifest *m);

/* Append -I/-L/-l flags from a cmake manifest to buf (space-separated,
 * no trailing NUL -- caller must add NUL before using as a C string). */
void pkg_cmake_manifest_append_cc_flags(const PkgCmakeManifest *m, Buf *buf);

/* ------------------------------------------------------------------ */
/* CLI entry points (called from main.c)                               */
/* ------------------------------------------------------------------ */

int cmd_pkg_new(int argc, char **argv);       /* tur new  */
int cmd_pkg_init(int argc, char **argv);      /* tur init */
int cmd_pkg_add(int argc, char **argv);       /* tur add  */
int cmd_pkg_add_cmake(int argc, char **argv); /* tur add-cmake */
int cmd_pkg_fetch(int argc, char **argv);     /* tur fetch */
int cmd_pkg_emit_cmake(int argc, char **argv); /* tur emit-cmake */
int cmd_pkg_install(int argc, char **argv);   /* tur install */
int cmd_pkg_uninstall(int argc, char **argv); /* tur uninstall */
int cmd_pkg_list(int argc, char **argv);      /* tur list */
int cmd_pkg_upgrade(int argc, char **argv);   /* tur upgrade */

#endif /* TUR_PKG_H */
