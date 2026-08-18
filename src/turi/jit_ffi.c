/* jit_ffi.c -- the c2mir-backed dynamic-FFI provider (jit-ffi-c2mir-plan,
 * F1).  Compiled into tur_jit_obj (JIT builds only), installed from the
 * host's startup via tur_jit_ffi_install; tur_core consumers reach it only
 * through the TurJitFfiProvider hook in jit_ffi_hook.c.
 *
 * A thunk is a ~10-line C function rendered from the signature string --
 * the exact cast-and-call the generated per-export __ffi shims bake in at
 * spice-build time, but synthesized at runtime for the precise signature,
 * so there is no --max-arity ceiling and no shape-table regeneration.  It
 * is compiled through the engine's own image path (c2mir -> MIR_link ->
 * MIR_gen), cached per signature, and kept for the process lifetime --
 * compile cost is milliseconds per UNIQUE signature, amortized against
 * tree-walking dispatch overhead.
 *
 * c2mir compile errors surface as a NULL thunk with a message in the
 * caller's errbuf (the engine already printed specifics to stderr); they
 * never abort. */
#include "jit_ffi.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jit_engine.h"

/* ------------------------------------------------------------------ */
/* signature -> C source                                               */
/* ------------------------------------------------------------------ */

/* Upper bound on thunked arity.  Not an ABI limit -- just a sanity cap on
 * the source buffer; raise freely if a real signature ever hits it. */
#define JIT_FFI_MAX_ARGS 256

static const char *class_c_type(char c) {
    switch (c) {
        case 'i': return "long long";
        case 'f': return "double";
        case 'F': return "float";
        case 'v': return "void";
        default:  return NULL;
    }
}

/* Validate "r:args" and return the arg count, or -1. */
static int sig_parse(const char *sig, char *out_ret, const char **out_args) {
    if (!sig || !sig[0] || sig[1] != ':') return -1;
    char r = sig[0];
    if (r != 'i' && r != 'f' && r != 'F' && r != 'v') return -1;
    const char *a = sig + 2;
    int n = 0;
    for (const char *p = a; *p; p++, n++) {
        if (*p != 'i' && *p != 'f' && *p != 'F') return -1;
        if (n >= JIT_FFI_MAX_ARGS) return -1;
    }
    *out_ret = r;
    *out_args = a;
    return n;
}

/* Render the thunk TU for `sig`.  Caller frees. */
static char *render_thunk_source(const char *sig, char ret, const char *args,
                                 int n) {
    (void)sig;
    size_t cap = 512 + (size_t)n * 48;
    char *src = (char *)malloc(cap);
    if (!src) return NULL;
    size_t pos = 0;
#define EMIT(...) pos += (size_t)snprintf(src + pos, cap - pos, __VA_ARGS__)
    EMIT("void __tur_ffi_thunk(void *fn, const long long *iv,\n"
         "                     const double *fv, void *sv,\n"
         "                     long long *out_i, double *out_f, void *out_s)\n"
         "{\n"
         "  (void)sv; (void)out_s; (void)iv; (void)fv;\n"
         "  (void)out_i; (void)out_f;\n  ");
    /* Store expression prefix by return class. */
    if (ret == 'i')      EMIT("*out_i = (long long)");
    else if (ret == 'f' || ret == 'F') EMIT("*out_f = (double)");
    /* The cast: ((RET (*)(A0, A1, ...))fn)(...) */
    EMIT("((%s (*)(", class_c_type(ret));
    if (n == 0) {
        EMIT("void");
    } else {
        for (int i = 0; i < n; i++)
            EMIT("%s%s", i ? ", " : "", class_c_type(args[i]));
    }
    EMIT("))fn)(");
    for (int i = 0; i < n; i++) {
        /* Position-indexed buffers: arg k reads iv[k] or fv[k] by class --
         * the same convention the interpreter's marshaller and the emitted
         * __ffi shims already share. */
        if (args[i] == 'i')      EMIT("%siv[%d]", i ? ", " : "", i);
        else if (args[i] == 'f') EMIT("%sfv[%d]", i ? ", " : "", i);
        else                     EMIT("%s(float)fv[%d]", i ? ", " : "", i);
    }
    EMIT(");\n}\n");
#undef EMIT
    if (pos >= cap) { free(src); return NULL; }   /* truncated: refuse */
    return src;
}

/* ------------------------------------------------------------------ */
/* signature cache                                                     */
/* ------------------------------------------------------------------ */

/* Chained hash keyed by the signature string.  Entries (and their images)
 * are process-lifetime by design, matching turi's closure policy; the
 * mutex is cheap insurance for the day the interpreter grows threads --
 * today's callers are single-threaded. */
typedef struct ThunkEntry {
    char              *sig;
    TurJitFfiThunkFn   thunk;   /* NULL = negative cache (compile failed) */
    struct ThunkEntry *next;
} ThunkEntry;

#define THUNK_BUCKETS 64
static ThunkEntry      *g_buckets[THUNK_BUCKETS];
static pthread_mutex_t  g_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned sig_hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h % THUNK_BUCKETS;
}

static TurJitFfiThunkFn thunk_for(const char *sig, char *errbuf,
                                  size_t errcap) {
    char ret;
    const char *args;
    int n = sig_parse(sig, &ret, &args);
    if (n < 0) {
        if (errbuf && errcap)
            snprintf(errbuf, errcap, "malformed ffi signature '%s'",
                     sig ? sig : "(null)");
        return NULL;
    }

    pthread_mutex_lock(&g_cache_lock);
    unsigned b = sig_hash(sig);
    for (ThunkEntry *e = g_buckets[b]; e; e = e->next) {
        if (strcmp(e->sig, sig) == 0) {
            TurJitFfiThunkFn t = e->thunk;
            pthread_mutex_unlock(&g_cache_lock);
            if (!t && errbuf && errcap)
                snprintf(errbuf, errcap,
                         "ffi thunk for '%s' failed to compile earlier", sig);
            return t;
        }
    }

    /* Miss: render + compile while holding the lock (the engine's compile
     * path is single-threaded state; see jit_engine.c's gen lock notes). */
    TurJitFfiThunkFn thunk = NULL;
    char *src = render_thunk_source(sig, ret, args, n);
    if (src) {
        TurJitImage *img = NULL;
        int rc = tur_jit_compile_image(src, strlen(src), NULL, NULL, 0, &img);
        if (rc == TUR_JIT_OK && img) {
            thunk = (TurJitFfiThunkFn)tur_jit_image_sym(img,
                                                        "__tur_ffi_thunk");
            /* The image stays resident for the process lifetime -- the
             * thunk pointer lives inside it. */
        }
        free(src);
    }

    ThunkEntry *e = (ThunkEntry *)calloc(1, sizeof(*e));
    if (e) {
        e->sig   = strdup(sig);
        e->thunk = thunk;   /* NULL entries negative-cache the failure */
        if (e->sig) {
            e->next = g_buckets[b];
            g_buckets[b] = e;
        } else {
            free(e);
        }
    }
    pthread_mutex_unlock(&g_cache_lock);

    if (!thunk && errbuf && errcap)
        snprintf(errbuf, errcap,
                 "c2mir could not compile an ffi thunk for '%s' "
                 "(see stderr for the compile diagnostic)", sig);
    return thunk;
}

static void *resolve_sym(const char *name) {
    if (!name) return NULL;
    return dlsym(RTLD_DEFAULT, name);
}

static const TurJitFfiProvider g_provider = { thunk_for, resolve_sym };

void tur_jit_ffi_install(void) {
    tur_jit_ffi_set_provider(&g_provider);
}
