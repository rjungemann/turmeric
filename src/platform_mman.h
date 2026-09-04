/* platform_mman.h - the sliver of <sys/mman.h> the interpreter actually uses.
 *
 * eval.c mmaps anonymous private memory for coroutine/continuation stacks.
 * That is exactly VirtualAlloc(MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE), so the
 * shim is a faithful one rather than a stand-in.
 *
 * Scope is deliberately narrow: only MAP_ANONYMOUS|MAP_PRIVATE with fd == -1
 * is supported, because that is the only way it is called here.  File-backed
 * mappings, MAP_FIXED, and mprotect are NOT emulated -- a caller reaching for
 * them would get silently wrong behaviour, so they abort instead.
 *
 * This lives apart from platform_fs.h on purpose: it needs <windows.h>, and
 * pulling that into every compiler TU invites macro collisions (ERROR, min/max)
 * for no benefit.  Only eval.c needs it.
 *
 * See docs/archive/windows-support-plan.md.
 */

#ifndef TUR_PLATFORM_MMAN_H
#define TUR_PLATFORM_MMAN_H

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

#include <stdio.h>
#include <stdlib.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

static inline void *tur_mmap(void *addr, size_t length, int prot, int flags,
                             int fd, long offset) {
    (void)addr; (void)offset;
    if (fd != -1 || !(flags & MAP_ANONYMOUS)) {
        fprintf(stderr, "turmeric: file-backed mmap is not supported on "
                        "Windows (platform_mman.h)\n");
        abort();
    }
    DWORD protect = (prot & PROT_EXEC)
                        ? ((prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE
                                               : PAGE_EXECUTE_READ)
                        : ((prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY);
    void *p = VirtualAlloc(NULL, (SIZE_T)length, MEM_COMMIT | MEM_RESERVE,
                           protect);
    return p != NULL ? p : MAP_FAILED;
}

static inline int tur_munmap(void *addr, size_t length) {
    /* MEM_RELEASE requires the size to be 0 and the address to be the exact
     * base returned by VirtualAlloc -- which it always is here, since we never
     * hand out interior pointers. */
    (void)length;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

#define mmap(addr, len, prot, flags, fd, off) \
    tur_mmap((addr), (len), (prot), (flags), (fd), (off))
#define munmap(addr, len) tur_munmap((addr), (len))

#endif /* _WIN32 */

#endif /* TUR_PLATFORM_MMAN_H */
