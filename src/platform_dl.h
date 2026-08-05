/* platform_dl.h - dlopen/dlsym/dlclose over the Win32 loader.
 *
 * MinGW has no <dlfcn.h>.  LoadLibrary/GetProcAddress/FreeLibrary are exact
 * counterparts, so this is a real implementation rather than a stub.
 *
 * The one thing that does not map cleanly is dlerror(): Win32 reports failures
 * through GetLastError(), a thread-local error CODE, whereas dlerror() returns
 * a human-readable STRING and clears the error as a side effect.  The shim
 * formats the code into a thread-local buffer and preserves the clear-on-read
 * behaviour, because callers rely on it -- spice_loader.c does
 * `dlerror(); p = dlsym(...); if (dlerror()) ...` to distinguish "symbol
 * resolved to NULL" from "symbol not found".
 *
 * On non-Windows this header is empty; include <dlfcn.h> there as usual.
 *
 * See docs/upcoming/v1/windows-support-plan.md.
 */

#ifndef TUR_PLATFORM_DL_H
#define TUR_PLATFORM_DL_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>    /* EnumProcessModules -- for the RTLD_DEFAULT walk */

#include <stdio.h>

/* dlopen flags have no Win32 counterpart -- LoadLibrary is always immediate
 * (RTLD_NOW) and never adds symbols to a global namespace (RTLD_LOCAL), which
 * is exactly what spice_loader.c asks for.  Accept and ignore them. */
#define RTLD_NOW    0x0002
#define RTLD_LAZY   0x0001
#define RTLD_LOCAL  0x0000
#define RTLD_GLOBAL 0x0100

/* dlsym(RTLD_DEFAULT, ...) means "search every image in the process, in load
 * order".  No single Win32 handle has that meaning, so use a sentinel that
 * cannot collide with a real HMODULE (an HMODULE is a mapped base address)
 * and give it its own path in tur_dlsym: the main executable first -- the
 * overwhelmingly common hit, since the JIT resolves runtime symbols linked
 * into tur.exe -- then every other loaded module, which is how dlfcn-win32
 * implements the same semantics.
 *
 * Caveat with no ELF counterpart: GetProcAddress sees only EXPORTED symbols,
 * and an executable exports nothing by default.  A caller relying on
 * RTLD_DEFAULT to find symbols in the main program must be linked with
 * -Wl,--export-all-symbols, or every lookup lands in the fallback walk and
 * misses.  (The JIT target does exactly that -- see src/CMakeLists.txt.) */
#define RTLD_DEFAULT ((void *)(intptr_t)-1)

/* Thread-local so two threads loading spices cannot scribble on each other's
 * pending error, matching dlerror()'s per-thread semantics. */
static __thread char  tur_dl_errbuf[512];
static __thread int   tur_dl_has_error = 0;

static inline void tur_dl_set_error(const char *op, const char *what) {
    DWORD code = GetLastError();
    char  msg[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, 0, msg, (DWORD)sizeof(msg) - 1, NULL);
    /* FormatMessage likes to append CRLF; trim it so the message sits inside a
     * caller's own sentence cleanly. */
    for (char *p = msg; *p; p++) {
        if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
    }
    snprintf(tur_dl_errbuf, sizeof(tur_dl_errbuf), "%s(%s): %s (error %lu)",
             op, what ? what : "", msg[0] ? msg : "unknown error",
             (unsigned long)code);
    tur_dl_has_error = 1;
}

static inline void *tur_dlopen(const char *path, int flags) {
    (void)flags;
    HMODULE h = LoadLibraryA(path);
    if (h == NULL) {
        tur_dl_set_error("LoadLibrary", path);
        return NULL;
    }
    return (void *)h;
}

static inline void *tur_dlsym(void *handle, const char *symbol) {
    FARPROC p = NULL;
    if (handle == RTLD_DEFAULT) {
        /* Process-wide search.  Main executable first, then every loaded
         * module.  EnumProcessModules over a fixed buffer: a truncated list
         * (more than 1024 modules) degrades to searching the first 1024,
         * which is already far beyond anything this process loads. */
        p = GetProcAddress(GetModuleHandleA(NULL), symbol);
        if (p == NULL) {
            HMODULE mods[1024];
            DWORD   needed = 0;
            if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods),
                                   &needed)) {
                DWORD n = needed / sizeof(HMODULE);
                if (n > 1024) n = 1024;
                for (DWORD i = 0; i < n && p == NULL; i++) {
                    p = GetProcAddress(mods[i], symbol);
                }
            }
        }
    } else {
        p = GetProcAddress((HMODULE)handle, symbol);
    }
    if (p == NULL) {
        tur_dl_set_error("GetProcAddress", symbol);
        return NULL;
    }
    /* A function pointer is not a void* in ISO C; the cast is the standard
     * (and unavoidable) idiom for a symbol-lookup API. */
    return (void *)(intptr_t)p;
}

static inline int tur_dlclose(void *handle) {
    /* dlclose returns 0 on success; FreeLibrary returns non-zero on success. */
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}

/* Returns the pending error and clears it, like dlerror(). */
static inline char *tur_dlerror(void) {
    if (!tur_dl_has_error) {
        return NULL;
    }
    tur_dl_has_error = 0;
    return tur_dl_errbuf;
}

#define dlopen(path, flags) tur_dlopen((path), (flags))
#define dlsym(handle, sym)  tur_dlsym((handle), (sym))
#define dlclose(handle)     tur_dlclose(handle)
#define dlerror()           tur_dlerror()

#endif /* _WIN32 */

#endif /* TUR_PLATFORM_DL_H */
