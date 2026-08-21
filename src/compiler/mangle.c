/* mangle.c -- shared C-identifier mangling for Turmeric binding names.
 * See mangle.h for the scheme and rationale (plan section A3 + the
 * reversible-name-mangling follow-up). */
#include "mangle.h"

#include <stdlib.h>
#include <string.h>

/* Two-letter mnemonic for a sigil/separator character, or NULL if `c` is not an
 * encoded byte. The three structural separators get mnemonics too:
 *   '_' -> "un", '-' -> "hy", '/' -> "sl"
 * so the encoding is injective and self-delimiting (a lone '_' in the output
 * always introduces an escape; "__" is reserved as a structural separator that
 * data can never produce).
 *
 * Mnemonics are exactly two lowercase letters and none begin with 'x' (which is
 * reserved for the "_xHH" hex escape), so the demangler can disambiguate. */
static const char *sigil_mnemonic(unsigned char c) {
    switch (c) {
        case '_':  return "un"; /* underscore (literal data underscore) */
        case '-':  return "hy"; /* hyphen     */
        case '/':  return "sl"; /* slash      */
        case '!':  return "ex"; /* exclamation */
        case '?':  return "qu"; /* question    */
        case '<':  return "lt";
        case '>':  return "gt";
        case '=':  return "eq";
        case '+':  return "pl"; /* plus    */
        case '*':  return "st"; /* star    */
        case '%':  return "pc"; /* percent */
        case '&':  return "am"; /* ampersand */
        case '|':  return "ba"; /* bar    */
        case '^':  return "cr"; /* caret  */
        case '~':  return "td"; /* tilde  */
        case '$':  return "dl"; /* dollar */
        case '@':  return "at";
        case '.':  return "do"; /* dot   */
        case ':':  return "cl"; /* colon */
        case '\'': return "qt"; /* quote/prime */
        case '#':  return "hs"; /* hash  */
        case ',':  return "cm"; /* comma */
        case ';':  return "sc"; /* semicolon */
        default:   return NULL;
    }
}

/* Inverse of sigil_mnemonic: map a two-letter mnemonic back to its source byte,
 * or -1 if the pair is not a known mnemonic. */
static int mnemonic_byte(char a, char b) {
    switch ((unsigned char)a << 8 | (unsigned char)b) {
        case 'u' << 8 | 'n': return '_';
        case 'h' << 8 | 'y': return '-';
        case 's' << 8 | 'l': return '/';
        case 'e' << 8 | 'x': return '!';
        case 'q' << 8 | 'u': return '?';
        case 'l' << 8 | 't': return '<';
        case 'g' << 8 | 't': return '>';
        case 'e' << 8 | 'q': return '=';
        case 'p' << 8 | 'l': return '+';
        case 's' << 8 | 't': return '*';
        case 'p' << 8 | 'c': return '%';
        case 'a' << 8 | 'm': return '&';
        case 'b' << 8 | 'a': return '|';
        case 'c' << 8 | 'r': return '^';
        case 't' << 8 | 'd': return '~';
        case 'd' << 8 | 'l': return '$';
        case 'a' << 8 | 't': return '@';
        case 'd' << 8 | 'o': return '.';
        case 'c' << 8 | 'l': return ':';
        case 'q' << 8 | 't': return '\'';
        case 'h' << 8 | 's': return '#';
        case 'c' << 8 | 'm': return ',';
        case 's' << 8 | 'c': return ';';
        default:             return -1;
    }
}

static int hex_digit(int v) { return v < 10 ? '0' + v : 'A' + (v - 10); }

static int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int tur_name_is_c_identifier(const char *name, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

size_t tur_mangle_bound(size_t src_len) {
    /* Worst case per source byte is the "_xHH" hex escape == 4 chars. */
    return src_len * 4 + 1;
}

void tur_mangle_append(char *dst, size_t *pk, const char *name, size_t len) {
    size_t k = *pk;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            /* Pure alphanumerics pass through unchanged. A literal '_' does NOT
             * pass through: it is encoded as "_un" so a lone '_' in the output
             * always introduces an escape (see mangle.h). */
            dst[k++] = (char)c;
        } else {
            const char *m = sigil_mnemonic(c);
            dst[k++] = '_';
            if (m) {
                dst[k++] = m[0];
                dst[k++] = m[1];
            } else {
                /* Escape hatch: any other byte as "_xHH". */
                dst[k++] = 'x';
                dst[k++] = (char)hex_digit((c >> 4) & 0xF);
                dst[k++] = (char)hex_digit(c & 0xF);
            }
        }
    }
    *pk = k;
}

void tur_mangle_legacy_append(char *dst, size_t *pk, const char *name,
                              size_t len) {
    size_t k = *pk;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            dst[k++] = (char)c;
        } else if (c == '-' || c == '/') {
            dst[k++] = '_';
        } else {
            const char *m = sigil_mnemonic(c);
            dst[k++] = '_';
            if (m && c != '_' && c != '-' && c != '/') {
                dst[k++] = m[0];
                dst[k++] = m[1];
            } else {
                dst[k++] = 'x';
                dst[k++] = (char)hex_digit((c >> 4) & 0xF);
                dst[k++] = (char)hex_digit(c & 0xF);
            }
        }
    }
    *pk = k;
}

void tur_mangle_ident(const char *name, char *out, size_t cap) {
    if (cap == 0) return;
    size_t k = 0;
    /* Mangle one source byte at a time so we can stop cleanly before the fixed
     * buffer overflows. tur_mangle_append treats each byte independently, so
     * per-byte and bulk mangling yield identical output. */
    for (const char *p = name; *p; p++) {
        char tmp[4];          /* worst case per byte is the "_xHH" escape == 4 */
        size_t tk = 0;
        tur_mangle_append(tmp, &tk, p, 1);
        if (k + tk >= cap) break;   /* leave room for the NUL terminator */
        memcpy(out + k, tmp, tk);
        k += tk;
    }
    out[k] = '\0';
}

size_t tur_demangle(const char *mangled, char *out, size_t cap) {
    /* Scan left to right; copy alnum; on "__" emit the structural separator
     * '/'; on a lone '_' read the next byte: 'x' -> two hex digits -> byte,
     * otherwise two letters -> reverse-mnemonic lookup. Output never grows, so a
     * `cap` >= strlen(mangled)+1 always suffices. Returns the number of bytes
     * written (excluding the NUL), or 0 on a malformed escape. */
    if (cap == 0) return 0;
    size_t k = 0;
    const char *p = mangled;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c != '_') {
            if (k + 1 >= cap) break;
            out[k++] = (char)c;
            p++;
            continue;
        }
        /* c == '_': an escape, a structural separator, or unterminated. */
        if (p[1] == '_') {
            /* Structural separator "__" -> '/'. */
            if (k + 1 >= cap) break;
            out[k++] = '/';
            p += 2;
            continue;
        }
        if (p[1] == 'x') {
            int hi = hex_value((unsigned char)p[2]);
            int lo = hex_value((unsigned char)p[3]);
            if (hi < 0 || lo < 0) { out[k] = '\0'; return 0; }
            if (k + 1 >= cap) break;
            out[k++] = (char)((hi << 4) | lo);
            p += 4;
            continue;
        }
        if (p[1] && p[2]) {
            int b = mnemonic_byte(p[1], p[2]);
            if (b < 0) { out[k] = '\0'; return 0; }
            if (k + 1 >= cap) break;
            out[k++] = (char)b;
            p += 3;
            continue;
        }
        /* Lone trailing '_' with no escape body: malformed. */
        out[k] = '\0';
        return 0;
    }
    out[k] = '\0';
    return k;
}

/* Curated denylist of libc / POSIX function symbols that a modern C toolchain's
 * system headers (unistd.h, stdio.h, stdlib.h, string.h, time.h, sys/socket.h,
 * ...) declare in a translation unit -- so a user top-level `defn` lowered to a
 * bare `static int64_t <name>(...)` of the SAME spelling is a redeclaration
 * conflict ("static declaration of 'X' follows non-static declaration").  The
 * defect is toolchain-dependent (e.g. macOS SDKs added `pipe2` to <unistd.h>,
 * turning a long-compiling fixture red) and platform-independent in principle,
 * so `raw_name_for_binding` mangles ONLY a bare (non-module-prefixed) global
 * whose spelling lands here.  Kept sorted for bsearch; extend it as real
 * collisions surface rather than pre-emptively (over-broad entries would mangle
 * a user's function for no reason).  See
 * docs/archive/codegen-user-defn-collides-with-libc-pipe2.md. */
static int sorted_strcmp(const void *key, const void *elem) {
    return strcmp((const char *)key, *(const char *const *)elem);
}

int tur_name_collides_libc(const char *name, size_t len) {
    if (!name || len == 0) return 0;
    /* Callers pass a NUL-terminated Symbol whose text is exactly `len` bytes;
     * a slice with an interior NUL or extra trailing bytes is not a real
     * identifier and cannot match a libc symbol -- reject it so the strcmp
     * below compares the whole spelling. */
    if (strlen(name) != len) return 0;
    /* DERIVED, not hand-grown.  This list used to be extended one report at a
     * time, which is why `div` (and eleven others) were still missing years
     * in: a hand-grown list only ever learns the names someone happened to
     * try.  It is now the set of lowercase identifiers *declared* by the
     * headers the generated TU includes -- stdio, stdlib, string, time,
     * unistd, fcntl, errno, setjmp, pthread, ucontext, sys/select,
     * sys/socket, netinet/in, arpa/inet -- unioned across the preprocessor
     * flag sets `tur build` compiles under, minus the C keywords (which
     * tur_name_is_c_keyword handles) and typedef names.
     *
     * Unioning across flags is load-bearing, not belt-and-braces: glibc
     * declares `gets` only from <bits/stdio2.h>, which stdio.h includes only
     * when __USE_FORTIFY_LEVEL > 0 -- i.e. only under -O2, which is exactly
     * what `tur build` passes and what a bare `cc -E` does not.  A
     * `(defn gets ...)` therefore compiled clean through `tur emit-c` and
     * failed under `tur run`.  Regenerate with:
     *
     *   for f in "" -O2 "-O2 -D_FORTIFY_SOURCE=2" "-O2 -D_FORTIFY_SOURCE=3" \
     *            "-O2 -std=c99" "-O2 -D_GNU_SOURCE"; do
     *     cc -E $f headers.c | grep -oE '\b[a-z][a-z0-9_]*[ \t]*\(' \
     *       | sed 's/[ \t]*(//'
     *   done | sort -u
     *
     * Over-matching is harmless -- a user name that does not actually collide
     * just gets its own `tur_u_` C symbol -- so prefer adding a name to
     * leaving one out.  Entries are host-derived (glibc), so a symbol unique
     * to another libc still wants adding by hand if it ever bites.
     *
     * Sorted for bsearch under strcmp. */
    static const char *const libc_names[] = {
        "a64l", "abort", "abs", "accept", "accept4", "access", "acct",
        "alarm", "aligned_alloc", "alloca", "arc4random", "arc4random_buf",
        "arc4random_uniform", "asctime", "asctime_r", "asprintf",
        "at_quick_exit", "atexit", "atof", "atoi", "atol", "atoll",
        "basename", "bcmp", "bcopy", "bind", "bindresvport",
        "bindresvport6", "brk", "bsearch", "bzero",
        "calloc", "canonicalize_file_name", "chdir", "chmod", "chown",
        "chroot", "clearenv", "clearerr", "clearerr_unlocked", "clock",
        "clock_adjtime", "clock_getcpuclockid", "clock_getres",
        "clock_gettime", "clock_nanosleep", "clock_settime", "clone",
        "close", "close_range", "closefrom", "confstr", "connect",
        "copy_file_range", "creat", "creat64", "crypt", "ctermid", "ctime",
        "ctime_r", "cuserid",
        "daemon", "difftime", "div", "dprintf", "drand48", "drand48_r",
        "dup", "dup2", "dup3", "dysize",
        "eaccess", "ecvt", "ecvt_r", "endusershell", "erand48", "erand48_r",
        "euidaccess", "execl", "execle", "execlp", "execv", "execve",
        "execveat", "execvp", "execvpe", "exit", "explicit_bzero",
        "faccessat", "fallocate", "fallocate64", "fchdir", "fchown",
        "fchownat", "fclose", "fcloseall", "fcntl", "fcntl64", "fcvt",
        "fcvt_r", "fdatasync", "fdopen", "feof", "feof_unlocked", "ferror",
        "ferror_unlocked", "fexecve", "fflush", "fflush_unlocked", "ffs",
        "ffsl", "ffsll", "fgetc", "fgetc_unlocked", "fgetpos", "fgetpos64",
        "fgets", "fgets_unlocked", "fileno", "fileno_unlocked", "flockfile",
        "fmemopen", "fopen", "fopen64", "fopencookie", "fork", "fpathconf",
        "fprintf", "fputc", "fputc_unlocked", "fputs", "fputs_unlocked",
        "fread", "fread_unlocked", "free", "freopen", "freopen64", "fscanf",
        "fseek", "fseeko", "fseeko64", "fsetpos", "fsetpos64", "fstat",
        "fsync", "ftell", "ftello", "ftello64", "ftruncate", "ftruncate64",
        "ftrylockfile", "funlockfile", "fwrite", "fwrite_unlocked",
        "gcvt", "get_current_dir_name", "getc", "getc_unlocked", "getchar",
        "getchar_unlocked", "getcontext", "getcpu", "getcwd", "getdate",
        "getdate_r", "getdelim", "getdomainname", "getdtablesize",
        "getegid", "getentropy", "getenv", "geteuid", "getgid", "getgroups",
        "gethostid", "gethostname", "getipv4sourcefilter", "getline",
        "getloadavg", "getlogin", "getlogin_r", "getopt", "getpagesize",
        "getpass", "getpeername", "getpgid", "getpgrp", "getpid", "getppid",
        "getpt", "getresgid", "getresuid", "gets", "getsid", "getsockname",
        "getsockopt", "getsourcefilter", "getsubopt", "gettid", "getuid",
        "getusershell", "getw", "getwd", "gmtime", "gmtime_r", "grantpt",
        "group_member",
        "htonl", "htons",
        "index", "inet6_opt_append", "inet6_opt_find", "inet6_opt_finish",
        "inet6_opt_get_val", "inet6_opt_init", "inet6_opt_next",
        "inet6_opt_set_val", "inet6_option_alloc", "inet6_option_append",
        "inet6_option_find", "inet6_option_init", "inet6_option_next",
        "inet6_option_space", "inet6_rth_add", "inet6_rth_getaddr",
        "inet6_rth_init", "inet6_rth_reverse", "inet6_rth_segments",
        "inet6_rth_space", "inet_addr", "inet_aton", "inet_lnaof",
        "inet_makeaddr", "inet_net_ntop", "inet_net_pton", "inet_neta",
        "inet_netof", "inet_network", "inet_nsap_addr", "inet_nsap_ntoa",
        "inet_ntoa", "inet_ntop", "inet_pton", "initstate", "initstate_r",
        "isatty", "isfdtype",
        "jrand48", "jrand48_r",
        "kill",
        "l64a", "labs", "lchown", "lcong48", "lcong48_r", "ldiv", "link",
        "linkat", "listen", "llabs", "lldiv", "localtime", "localtime_r",
        "lockf", "lockf64", "longjmp", "lrand48", "lrand48_r", "lseek",
        "lseek64", "lstat",
        "makecontext", "malloc", "mblen", "mbstowcs", "mbtowc", "memccpy",
        "memchr", "memcmp", "memcpy", "memfrob", "memmem", "memmove",
        "mempcpy", "memrchr", "memset", "mkdir", "mkdtemp", "mkostemp",
        "mkostemp64", "mkostemps", "mkostemps64", "mkstemp", "mkstemp64",
        "mkstemps", "mkstemps64", "mktemp", "mktime", "mrand48",
        "mrand48_r",
        "name_to_handle_at", "nanosleep", "nice", "nrand48", "nrand48_r",
        "ntohl", "ntohs",
        "obstack_printf", "obstack_vprintf", "on_exit", "open", "open64",
        "open_by_handle_at", "open_memstream", "openat", "openat64",
        "pathconf", "pause", "pclose", "perror", "pipe", "pipe2", "poll",
        "popen", "posix_fadvise", "posix_fadvise64", "posix_fallocate",
        "posix_fallocate64", "posix_memalign", "posix_openpt", "pread",
        "pread64", "printf", "profil", "pselect", "pthread_atfork",
        "pthread_attr_destroy", "pthread_attr_getaffinity_np",
        "pthread_attr_getdetachstate", "pthread_attr_getguardsize",
        "pthread_attr_getinheritsched", "pthread_attr_getschedparam",
        "pthread_attr_getschedpolicy", "pthread_attr_getscope",
        "pthread_attr_getsigmask_np", "pthread_attr_getstack",
        "pthread_attr_getstackaddr", "pthread_attr_getstacksize",
        "pthread_attr_init", "pthread_attr_setaffinity_np",
        "pthread_attr_setdetachstate", "pthread_attr_setguardsize",
        "pthread_attr_setinheritsched", "pthread_attr_setschedparam",
        "pthread_attr_setschedpolicy", "pthread_attr_setscope",
        "pthread_attr_setsigmask_np", "pthread_attr_setstack",
        "pthread_attr_setstackaddr", "pthread_attr_setstacksize",
        "pthread_barrier_destroy", "pthread_barrier_init",
        "pthread_barrier_wait", "pthread_barrierattr_destroy",
        "pthread_barrierattr_getpshared", "pthread_barrierattr_init",
        "pthread_barrierattr_setpshared", "pthread_cancel",
        "pthread_clockjoin_np", "pthread_cond_broadcast",
        "pthread_cond_clockwait", "pthread_cond_destroy",
        "pthread_cond_init", "pthread_cond_signal",
        "pthread_cond_timedwait", "pthread_cond_wait",
        "pthread_condattr_destroy", "pthread_condattr_getclock",
        "pthread_condattr_getpshared", "pthread_condattr_init",
        "pthread_condattr_setclock", "pthread_condattr_setpshared",
        "pthread_create", "pthread_detach", "pthread_equal", "pthread_exit",
        "pthread_getaffinity_np", "pthread_getattr_default_np",
        "pthread_getattr_np", "pthread_getconcurrency",
        "pthread_getcpuclockid", "pthread_getname_np",
        "pthread_getschedparam", "pthread_getspecific", "pthread_join",
        "pthread_key_create", "pthread_key_delete",
        "pthread_mutex_clocklock", "pthread_mutex_consistent",
        "pthread_mutex_consistent_np", "pthread_mutex_destroy",
        "pthread_mutex_getprioceiling", "pthread_mutex_init",
        "pthread_mutex_lock", "pthread_mutex_setprioceiling",
        "pthread_mutex_timedlock", "pthread_mutex_trylock",
        "pthread_mutex_unlock", "pthread_mutexattr_destroy",
        "pthread_mutexattr_getprioceiling", "pthread_mutexattr_getprotocol",
        "pthread_mutexattr_getpshared", "pthread_mutexattr_getrobust",
        "pthread_mutexattr_getrobust_np", "pthread_mutexattr_gettype",
        "pthread_mutexattr_init", "pthread_mutexattr_setprioceiling",
        "pthread_mutexattr_setprotocol", "pthread_mutexattr_setpshared",
        "pthread_mutexattr_setrobust", "pthread_mutexattr_setrobust_np",
        "pthread_mutexattr_settype", "pthread_once",
        "pthread_rwlock_clockrdlock", "pthread_rwlock_clockwrlock",
        "pthread_rwlock_destroy", "pthread_rwlock_init",
        "pthread_rwlock_rdlock", "pthread_rwlock_timedrdlock",
        "pthread_rwlock_timedwrlock", "pthread_rwlock_tryrdlock",
        "pthread_rwlock_trywrlock", "pthread_rwlock_unlock",
        "pthread_rwlock_wrlock", "pthread_rwlockattr_destroy",
        "pthread_rwlockattr_getkind_np", "pthread_rwlockattr_getpshared",
        "pthread_rwlockattr_init", "pthread_rwlockattr_setkind_np",
        "pthread_rwlockattr_setpshared", "pthread_self",
        "pthread_setaffinity_np", "pthread_setattr_default_np",
        "pthread_setcancelstate", "pthread_setcanceltype",
        "pthread_setconcurrency", "pthread_setname_np",
        "pthread_setschedparam", "pthread_setschedprio",
        "pthread_setspecific", "pthread_spin_destroy", "pthread_spin_init",
        "pthread_spin_lock", "pthread_spin_trylock", "pthread_spin_unlock",
        "pthread_testcancel", "pthread_timedjoin_np", "pthread_tryjoin_np",
        "pthread_yield", "ptsname", "ptsname_r", "putc", "putc_unlocked",
        "putchar", "putchar_unlocked", "putenv", "puts", "putw", "pwrite",
        "pwrite64",
        "qecvt", "qecvt_r", "qfcvt", "qfcvt_r", "qgcvt", "qsort", "qsort_r",
        "quick_exit",
        "raise", "rand", "rand_r", "random", "random_r", "rawmemchr",
        "read", "readahead", "readlink", "readlinkat", "realloc",
        "reallocarray", "realpath", "recv", "recvfrom", "recvmmsg",
        "recvmsg", "remove", "rename", "renameat", "renameat2", "revoke",
        "rewind", "rindex", "rmdir", "rpmatch",
        "sbrk", "scanf", "sched_get_priority_max", "sched_get_priority_min",
        "sched_getaffinity", "sched_getcpu", "sched_getparam",
        "sched_getscheduler", "sched_rr_get_interval", "sched_setaffinity",
        "sched_setparam", "sched_setscheduler", "sched_yield",
        "secure_getenv", "seed48", "seed48_r", "select", "send", "sendmmsg",
        "sendmsg", "sendto", "setbuf", "setbuffer", "setcontext",
        "setdomainname", "setegid", "setenv", "seteuid", "setgid",
        "sethostid", "sethostname", "setipv4sourcefilter", "setjmp",
        "setlinebuf", "setlogin", "setns", "setpgid", "setpgrp", "setregid",
        "setresgid", "setresuid", "setreuid", "setsid", "setsockopt",
        "setsourcefilter", "setstate", "setstate_r", "setuid",
        "setusershell", "setvbuf", "shutdown", "sigabbrev_np",
        "sigdescr_np", "siglongjmp", "signal", "sleep", "snprintf",
        "sockatmark", "socket", "socketpair", "splice", "sprintf", "srand",
        "srand48", "srand48_r", "srandom", "srandom_r", "sscanf", "stat",
        "stpcpy", "stpncpy", "strcasecmp", "strcasecmp_l", "strcasestr",
        "strcat", "strchr", "strchrnul", "strcmp", "strcoll", "strcoll_l",
        "strcpy", "strcspn", "strdup", "strerror", "strerror_l",
        "strerror_r", "strerrordesc_np", "strerrorname_np", "strfromd",
        "strfromf", "strfromf128", "strfromf32", "strfromf32x",
        "strfromf64", "strfromf64x", "strfroml", "strfry", "strftime",
        "strftime_l", "strlcat", "strlcpy", "strlen", "strncasecmp",
        "strncasecmp_l", "strncat", "strncmp", "strncpy", "strndup",
        "strnlen", "strpbrk", "strptime", "strptime_l", "strrchr", "strsep",
        "strsignal", "strspn", "strstr", "strtod", "strtod_l", "strtof",
        "strtof128", "strtof128_l", "strtof32", "strtof32_l", "strtof32x",
        "strtof32x_l", "strtof64", "strtof64_l", "strtof64x", "strtof64x_l",
        "strtof_l", "strtok", "strtok_r", "strtol", "strtol_l", "strtold",
        "strtold_l", "strtoll", "strtoll_l", "strtoq", "strtoul",
        "strtoul_l", "strtoull", "strtoull_l", "strtouq", "strverscmp",
        "strxfrm", "strxfrm_l", "swab", "swapcontext", "symlink",
        "symlinkat", "sync", "sync_file_range", "syncfs", "syscall",
        "sysconf", "system",
        "tcgetpgrp", "tcsetpgrp", "tee", "tempnam", "time", "timegm",
        "timelocal", "timer_create", "timer_delete", "timer_getoverrun",
        "timer_gettime", "timer_settime", "times", "timespec_get",
        "timespec_getres", "tmpfile", "tmpfile64", "tmpnam", "tmpnam_r",
        "truncate", "truncate64", "ttyname", "ttyname_r", "ttyslot",
        "tzset",
        "ualarm", "ungetc", "unlink", "unlinkat", "unlockpt", "unsetenv",
        "unshare", "usleep",
        "valloc", "vasprintf", "vdprintf", "vfork", "vfprintf", "vfscanf",
        "vhangup", "vmsplice", "vprintf", "vscanf", "vsnprintf", "vsprintf",
        "vsscanf",
        "wait", "waitpid", "wcstombs", "wctomb", "write",
    };
    return bsearch(name, libc_names,
                   sizeof(libc_names) / sizeof(libc_names[0]),
                   sizeof(libc_names[0]), sorted_strcmp) != NULL;
}

int tur_name_is_c_keyword(const char *name, size_t len) {
    if (!name || len == 0) return 0;
    /* Same slice guard as tur_name_collides_libc: an interior NUL or trailing
     * bytes mean this is not a whole identifier, so it cannot be a keyword. */
    if (strlen(name) != len) return 0;
    /* Every C reserved word, C89 through C23, plus `asm` / `typeof` (which
     * every toolchain we target accepts).  Sorted for bsearch under strcmp,
     * where the leading '_' (0x5F) of the C99/C11 spellings sorts before every
     * lowercase letter, so the `_X` block comes first.  Unlike the libc list
     * this one is fixed by the standard rather than derived from one host's
     * headers, and a missing entry is the same unreadable cc cascade the
     * guard exists to prevent. */
    static const char *const c_keywords[] = {
        "_Alignas", "_Alignof", "_Atomic", "_BitInt", "_Bool", "_Complex",
        "_Decimal128", "_Decimal32", "_Decimal64", "_Generic", "_Imaginary",
        "_Noreturn", "_Static_assert", "_Thread_local",
        "alignas", "alignof", "asm", "auto",
        "bool", "break",
        "case", "char", "const", "constexpr", "continue",
        "default", "do", "double",
        "else", "enum", "extern",
        "false", "float", "for",
        "goto",
        "if", "inline", "int",
        "long",
        "nullptr",
        "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "static_assert", "struct",
        "switch",
        "thread_local", "true", "typedef", "typeof", "typeof_unqual",
        "union", "unsigned",
        "void", "volatile",
        "while",
    };
    return bsearch(name, c_keywords,
                   sizeof(c_keywords) / sizeof(c_keywords[0]),
                   sizeof(c_keywords[0]), sorted_strcmp) != NULL;
}
