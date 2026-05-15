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
    bool  optional;
} PkgSpice;

/* ------------------------------------------------------------------ */
/* The parsed content of a build.tur file                              */
/* ------------------------------------------------------------------ */

typedef struct PkgManifest {
    char        *name;
    char        *version;
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
    bool         no_stdlib;
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

/* Parse a build.tur file.  All returned strings are heap-allocated.
 * Returns true on success; prints diagnostics to stderr on failure. */
bool pkg_manifest_read(const char *path, PkgManifest *out);

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

/* Compare two semver strings.  Returns <0, 0, or >0 like strcmp. */
int pkg_semver_compare(const char *a, const char *b);

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
bool pkg_cmake_build(const char *project_dir,
                     const PkgManifest *manifest,
                     PkgLockFile *lock);

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

#endif /* TUR_PKG_H */
