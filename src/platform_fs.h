/* platform_fs.h - POSIX shims for the Windows (MinGW-w64 / UCRT64) build.
 *
 * MinGW supplies far more of POSIX than MSVC does -- <unistd.h>, <dirent.h>,
 * <sys/stat.h> and winpthreads all exist -- so this header only has to cover
 * the genuine gaps.  Keep it that way: add a shim here ONLY after the compiler
 * has actually told you the symbol is missing.  Speculative aliasing is how a
 * compat header turns into a second, subtly-wrong libc.
 *
 * On every non-Windows platform this header is empty, so it is safe (and cheap)
 * to include unconditionally.
 *
 * See docs/archive/windows-support-plan.md (WIN0).
 */

#ifndef TUR_PLATFORM_FS_H
#define TUR_PLATFORM_FS_H

#ifdef _WIN32

#include <direct.h>   /* _mkdir  */
#include <errno.h>
#include <fcntl.h>    /* open, O_CREAT/O_EXCL -- for mkstemps */
#include <limits.h>   /* PATH_MAX */
#include <process.h>  /* _getpid -- for mkstemps' seed */
#include <stdio.h>    /* FILE, fgetc -- for getline */
#include <stdlib.h>   /* _fullpath, _putenv_s, getenv, malloc, realloc */
#include <string.h>   /* memcpy, strlen */
#include <sys/types.h>/* ssize_t */
#include <time.h>     /* time -- for mkstemps' seed */
#include <sys/stat.h> /* stat -- used for realpath's existence check.  NOT
                       * _access(): the underscore-prefixed CRT names in <io.h>
                       * are hidden under -std=c11 -pedantic (__STRICT_ANSI__),
                       * whereas stat() is visible and does the same job. */

#ifndef PATH_MAX
#define PATH_MAX 260  /* MAX_PATH; MinGW normally defines PATH_MAX in limits.h */
#endif

/* ---- Filesystem ---------------------------------------------------------- */

/*
 * realpath(3) -> _fullpath().
 *
 * Two differences from the POSIX call have to be papered over:
 *
 *   - Argument order is reversed (_fullpath takes the destination first), and
 *     it wants an explicit buffer size.
 *   - _fullpath canonicalises purely lexically and succeeds on a path that does
 *     not exist, whereas realpath() resolves against the filesystem and fails
 *     with ENOENT.  Callers here use a NULL return to mean "not a real file"
 *     (e.g. load_path_key in elab_toplevel.c dedupes modules by resolved path),
 *     so the existence check is re-added explicitly -- without it, a typo'd
 *     import would canonicalise happily instead of being rejected.
 *
 * Passing resolved == NULL is supported, matching realpath(path, NULL): the CRT
 * mallocs the buffer and the caller owns it.
 */
static inline char *tur_realpath(const char *path, char *resolved) {
    struct stat st;
    char *result = _fullpath(resolved, path, PATH_MAX);
    if (result == NULL) {
        return NULL;
    }
    if (stat(result, &st) != 0) {
        /* Only free what _fullpath allocated for us; a caller-supplied buffer
         * is not ours to release. */
        if (resolved == NULL) {
            free(result);
        }
        return NULL;
    }
    return result;
}
#define realpath(path, resolved) tur_realpath((path), (resolved))

/*
 * mkdir(2) -- MinGW declares the legacy MSVC one-argument form
 * (int mkdir(const char *)), so the POSIX mode argument has to be dropped.
 * That is the correct behaviour, not a loss: Windows has no POSIX permission
 * bits, and the ACL a new directory inherits is what we want anyway.
 */
#define mkdir(path, mode) ((void)(mode), _mkdir(path))

/* ---- Symlinks -------------------------------------------------------------
 *
 * Windows symlinks require either Developer Mode or an elevated process, so
 * `tur install`'s symlink-based bin-shims (install.c) do not work here.  That
 * is deferred -- WIN4 in the plan -- and these shims exist so the file compiles
 * and FAILS CLEANLY rather than silently doing the wrong thing:
 *
 *   - Nothing is ever reported as a symlink, so inst_check_bin_target() sees a
 *     plain file and refuses to clobber it (the safe answer).
 *   - symlink() reports ENOSYS, so inst_replace_symlink() prints its existing
 *     "symlink(...) failed: Function not implemented" diagnostic.
 *
 * Do NOT "fix" these by making symlink() succeed via CreateSymbolicLinkA
 * without also handling the privilege requirement -- a shim that works only for
 * elevated users is worse than one that never works.
 */
#define lstat(path, buf) stat((path), (buf))
#define S_ISLNK(mode)    (0)

static inline long tur_readlink(const char *path, char *buf, size_t bufsiz) {
    (void)path; (void)buf; (void)bufsiz;
    errno = EINVAL;  /* POSIX: EINVAL == "not a symbolic link" */
    return -1;
}
#define readlink(path, buf, bufsiz) tur_readlink((path), (buf), (bufsiz))

static inline int tur_symlink(const char *target, const char *linkpath) {
    (void)target; (void)linkpath;
    errno = ENOSYS;
    return -1;
}
#define symlink(target, linkpath) tur_symlink((target), (linkpath))

/* ---- Environment --------------------------------------------------------- */

/*
 * setenv(3) -> _putenv_s().  _putenv_s always overwrites, so the POSIX
 * overwrite==0 ("only set if absent") case has to be handled by hand.
 */
static inline int tur_setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name) != NULL) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#define setenv(name, value, overwrite) tur_setenv((name), (value), (overwrite))

/*
 * unsetenv(3) -> _putenv_s(name, "").  Assigning the empty string is how the
 * CRT removes a variable outright -- after it, getenv(name) returns NULL, not
 * "".  POSIX unsetenv succeeds when the name is already absent, and so does
 * this.  (EINVAL for a NULL/empty/'='-bearing name matches POSIX too.)
 */
static inline int tur_unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }
    return _putenv_s(name, "") == 0 ? 0 : -1;
}
#define unsetenv(name) tur_unsetenv((name))

/* ---- Temp files ---------------------------------------------------------- */

/*
 * mkstemps(3) -- BSD/glibc.  MinGW has mkstemp() but not the "-s" variant that
 * keeps a fixed suffix (e.g. "/tmp/tur-fmtXXXXXX.tur"), which is what callers
 * here need: the emitted file must keep its .tur/.c extension to be recognised
 * downstream.
 *
 * O_EXCL is what makes this safe -- the create fails rather than clobbers if we
 * lose a race -- so a weak name generator only costs retries, never
 * correctness.  Seeded per call from pid ^ time so two processes starting in
 * the same second do not walk the same sequence.
 */
static inline int tur_mkstemps(char *tmpl, int suffixlen) {
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    size_t len = strlen(tmpl);
    if (suffixlen < 0 || (size_t)suffixlen + 6 > len) {
        errno = EINVAL;
        return -1;
    }
    char *slot = tmpl + len - (size_t)suffixlen - 6;
    for (int i = 0; i < 6; i++) {
        if (slot[i] != 'X') {
            errno = EINVAL;
            return -1;
        }
    }
    unsigned int seed = (unsigned int)_getpid() ^ (unsigned int)time(NULL);
    for (int attempt = 0; attempt < 128; attempt++) {
        for (int i = 0; i < 6; i++) {
            seed = seed * 1103515245u + 12345u;
            slot[i] = letters[(seed >> 16) % (sizeof(letters) - 1)];
        }
        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL | O_BINARY, 0600);
        if (fd >= 0) {
            return fd;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    errno = EEXIST;
    return -1;
}
#define mkstemps(tmpl, suffixlen) tur_mkstemps((tmpl), (suffixlen))

/* ---- Strings ------------------------------------------------------------- */

/*
 * getline(3) -- POSIX.1-2008; absent from the UCRT.  Grows *lineptr as needed
 * and returns the byte count excluding the NUL (the trailing newline, if any,
 * IS included, as POSIX specifies).  Returns -1 at EOF with nothing read.
 *
 * On realloc failure the already-grown buffer is still written back through the
 * lineptr and n out-params so the caller can free it -- dropping it here would
 * leak.
 */
static inline ssize_t tur_getline(char **lineptr, size_t *n, FILE *stream) {
    if (lineptr == NULL || n == NULL || stream == NULL) {
        errno = EINVAL;
        return -1;
    }
    char  *buf = *lineptr;
    size_t cap = *n;
    if (buf == NULL || cap == 0) {
        cap = 128;
        buf = (char *)malloc(cap);
        if (buf == NULL) {
            return -1;
        }
    }
    size_t len = 0;
    int    c;
    while ((c = fgetc(stream)) != EOF) {
        if (len + 1 >= cap) {
            size_t newcap = cap * 2;
            char  *grown  = (char *)realloc(buf, newcap);
            if (grown == NULL) {
                *lineptr = buf;
                *n       = cap;
                return -1;
            }
            buf = grown;
            cap = newcap;
        }
        buf[len++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    *lineptr = buf;
    *n       = cap;
    if (len == 0) {
        return -1;  /* EOF with nothing read */
    }
    buf[len] = '\0';
    return (ssize_t)len;
}
#define getline(lineptr, n, stream) tur_getline((lineptr), (n), (stream))

/*
 * strndup(3) -- POSIX.1-2008, present in glibc but not in the UCRT.
 * Copies at most n bytes, stopping early at a NUL, and always NUL-terminates.
 */
static inline char *tur_strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len] != '\0') {
        len++;
    }
    char *copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}
#define strndup(s, n) tur_strndup((s), (n))

/* ---- Process exit status --------------------------------------------------
 *
 * <sys/wait.h> does not exist on Windows, and it is not needed: the POSIX
 * wait-status encoding it exists to decode is a POSIX invention.  Windows'
 * system() returns the child's exit code directly, so the accessors collapse to
 * the identity.  (These are only ever applied to a system() return value here --
 * there is no waitpid() on this path.)
 */
#ifndef WIFEXITED
#define WIFEXITED(status)   (1)
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(status) (status)
#endif
/* Windows has no signals in the POSIX sense, so a child can never be reported as
 * signal-terminated.  WIFSIGNALED is therefore always false, which makes
 * WTERMSIG dead code -- it is defined only so the call sites compile. */
#ifndef WIFSIGNALED
#define WIFSIGNALED(status) (0)
#endif
#ifndef WTERMSIG
#define WTERMSIG(status)    (0)
#endif

#endif /* _WIN32 */

/* Everything below is cross-platform.  These includes are outside the _WIN32
 * block on purpose: the helpers that follow are compiled on every platform, so
 * they cannot rely on the Windows-only includes above. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- Temp directory (all platforms) ---------------------------------------
 *
 * "/tmp" cannot be hardcoded: on Windows a leading "/" means "root of the
 * current drive", so "/tmp/tur-build" resolves to C:\tmp\tur-build -- a
 * directory that does not exist on a stock install and whose parent we have no
 * business creating.  Every temp path must therefore be built from here.
 *
 * The Windows lookup order (TMP, TEMP, USERPROFILE) is the same one
 * GetTempPathA uses, done with getenv so this header does not have to drag
 * <windows.h> into every compiler TU.
 *
 * The returned path has no trailing separator, so callers can always append
 * "/name".  Forward slashes are fine in Win32 file APIs, so the mixed
 * "C:\Users\x\Temp/tur-abc123.c" that results is valid.
 */
static inline const char *tur_temp_dir(void) {
    static char cached[512];
    if (cached[0] != '\0') {
        return cached;
    }
    const char *dir = NULL;
#ifdef _WIN32
    dir = getenv("TMP");
    if (dir == NULL || *dir == '\0') dir = getenv("TEMP");
    if (dir == NULL || *dir == '\0') dir = getenv("USERPROFILE");
    if (dir == NULL || *dir == '\0') dir = ".";
#else
    dir = getenv("TMPDIR");
    if (dir == NULL || *dir == '\0') dir = "/tmp";
#endif
    snprintf(cached, sizeof(cached), "%s", dir);
    size_t n = strlen(cached);
    while (n > 1 && (cached[n - 1] == '/' || cached[n - 1] == '\\')) {
        cached[--n] = '\0';
    }
    return cached;
}

/* ---- Executables and shell quoting (all platforms) ------------------------
 *
 * TUR_SHQ -- the quote character for a path inside a system() command string.
 * POSIX sh takes '...'; cmd.exe treats a single quote as an ordinary character
 * and chokes on the result with "The filename, directory name, or volume label
 * syntax is incorrect."  Anything handed to system() must use this.
 */
#ifdef _WIN32
#define TUR_SHQ "\""
#else
#define TUR_SHQ "'"
#endif

/*
 * Give an executable path the platform's extension.
 *
 * The MinGW linker appends ".exe" to any -o name that lacks an extension.  So a
 * path handed to cc as "X" comes back as "X.exe" -- and Windows' execv()/system()
 * do NOT fall back to the .exe name the way MSYS bash does.  Every site that
 * builds an executable and then runs it must therefore agree on the name, or the
 * build silently succeeds and the exec silently fails.
 *
 * Idempotent, so it is safe to apply more than once along a path.
 */
static inline void tur_exe_path(char *path, size_t cap) {
#ifdef _WIN32
    size_t n = strlen(path);
    if (n >= 4 && _stricmp(path + n - 4, ".exe") == 0) {
        return;
    }
    if (n + 4 < cap) {
        memcpy(path + n, ".exe", 5);
    }
#else
    (void)path;
    (void)cap;
#endif
}

/*
 * Make `-o <out>` really produce a file at <out>.
 *
 * MinGW's linker appends ".exe" to any -o name that has no extension, so
 * `cc -o /tmp/tur-test-abc123` silently writes /tmp/tur-test-abc123.exe and
 * leaves nothing at the name the caller asked for.  Callers then exec the path
 * they passed and get either "not found" or -- worse -- the empty placeholder
 * that mktemp(1) created there, which execs as a zero-byte file and produces no
 * output and no error.  (That is exactly how tests/run.sh fails on Windows: it
 * mktemps $exe, builds to $exe, and runs $exe.)
 *
 * Rather than teach every call site about ".exe", settle it here: after a
 * successful link, move <out>.exe back onto <out>.  A PE file is executable
 * regardless of its extension when launched by full path, which is how every
 * caller here launches it.  No-op on non-Windows, and no-op when the caller
 * already asked for a .exe.
 *
 * Returns 0 on success (including the no-op cases), -1 on a failed move.
 */
static inline int tur_settle_exe_output(const char *out_path) {
#ifdef _WIN32
    struct stat st;
    char        linked[1024];
    size_t      n = strlen(out_path);
    if (n >= 4 && _stricmp(out_path + n - 4, ".exe") == 0) {
        return 0;  /* caller asked for .exe; the linker wrote exactly that */
    }
    if (n + 5 > sizeof(linked)) {
        return -1;
    }
    snprintf(linked, sizeof(linked), "%s.exe", out_path);
    if (stat(linked, &st) != 0) {
        return 0;  /* linker did not append after all -- nothing to settle */
    }
    remove(out_path);  /* drop mktemp's empty placeholder, if any */
    return rename(linked, out_path);
#else
    (void)out_path;
    return 0;
#endif
}

/* ---- Shared-library file naming -------------------------------------------
 *
 * ELF platforms want a `lib` prefix on a shared object -- it is the convention
 * the dynamic loader searches by, and `-l<name>` is defined in terms of it.  PE
 * has no such convention: a Windows module is `<name>.dll`, full stop.  A
 * `libfoo.so` written on Windows is a file LoadLibrary will happily open by
 * absolute path, but that nothing which *discovers* modules by name -- Godot's
 * GDExtension loader among them -- will recognise, so `tur build --shared` has
 * to produce the platform's real spelling rather than one that merely loads.
 *
 * Both halves are string literals so a call site can paste them into a format
 * string with no runtime branch:
 *
 *     snprintf(p, n, "%s/lib/" TUR_SHLIB_PREFIX "%s" TUR_SHLIB_EXT, dir, base);
 *
 * macOS deliberately keeps `lib<name>.so` rather than the `.dylib` its linker
 * would name a library itself.  clang's `-shared` produces a valid Mach-O dylib
 * under any name, dlopen does not care about the extension, and downstream
 * consumers (the turmeric-godot shim's `.gdextension` entries) already spell it
 * `.so`.  Renaming it would be a gratuitous break for no functional gain.
 */
#ifdef _WIN32
#  define TUR_SHLIB_PREFIX ""
#  define TUR_SHLIB_EXT    ".dll"
#else
#  define TUR_SHLIB_PREFIX "lib"
#  define TUR_SHLIB_EXT    ".so"
#endif

/* ---- Directory entry kind (all platforms) ---------------------------------
 *
 * struct dirent::d_type is a BSD/glibc extension, not POSIX.  MinGW omits the
 * field entirely, and even where it exists a filesystem is allowed to answer
 * DT_UNKNOWN and make the caller go and look.  Callers therefore cannot read
 * d_type directly without either breaking on Windows or silently mis-skipping
 * entries on filesystems that decline to answer.
 *
 * These helpers give a definite answer everywhere: they trust d_type when it is
 * present and conclusive, and fall back to stat() when it is not.  Defined for
 * every platform (not just Windows) precisely so the call sites stay uniform.
 */

static inline int tur_dirent_stat_mode(const char *dir, const char *name,
                                       unsigned int *mode_out) {
    char        path[4096];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (stat(path, &st) != 0) {
        return -1;
    }
    *mode_out = (unsigned int)st.st_mode;
    return 0;
}

static inline int tur_dirent_is_dir(const char *dir, const struct dirent *ent) {
#ifdef DT_DIR
    if (ent->d_type == DT_DIR) return 1;
    if (ent->d_type != DT_UNKNOWN) return 0;
#endif
    unsigned int mode;
    if (tur_dirent_stat_mode(dir, ent->d_name, &mode) != 0) return 0;
    return S_ISDIR(mode) ? 1 : 0;
}

static inline int tur_dirent_is_reg(const char *dir, const struct dirent *ent) {
#ifdef DT_REG
    if (ent->d_type == DT_REG) return 1;
    if (ent->d_type != DT_UNKNOWN) return 0;
#endif
    unsigned int mode;
    if (tur_dirent_stat_mode(dir, ent->d_name, &mode) != 0) return 0;
    return S_ISREG(mode) ? 1 : 0;
}

#endif /* TUR_PLATFORM_FS_H */
