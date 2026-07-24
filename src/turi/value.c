#include "value.h"
#include "env.h"     /* TuriEnv layout + value_scratch/value_perm pools */
#include "arena.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TuriClosure is an incomplete type here; value.c only uses it as a pointer. */

/* ---------------------------------------------------------------------------
 * Env-owned value-payload pool
 * --------------------------------------------------------------------------- */

void *turi_val_alloc(TuriEnv *env, size_t n) {
    return arena_alloc(&env->value_scratch, n);
}

void *turi_val_calloc(TuriEnv *env, size_t n) {
    void *p = arena_alloc(&env->value_scratch, n);
    if (n) memset(p, 0, n);
    return p;
}

void *turi_val_calloc_aligned(TuriEnv *env, size_t n, size_t align) {
    void *p = arena_alloc_aligned(&env->value_scratch, n, align);
    if (n) memset(p, 0, n);
    return p;
}

char *turi_val_strdup(TuriEnv *env, const char *s) {
    if (!s) return NULL;
    return arena_strdup(&env->value_scratch, s, strlen(s));
}

/* turi-value-pool-scratch-promotion-plan: permanent-pool variants used only by
 * the promotion walk to relocate escaping values out of scratch. */
void *turi_val_perm_alloc(TuriEnv *env, size_t n) {
    return arena_alloc(&env->value_perm, n);
}

void *turi_val_perm_calloc(TuriEnv *env, size_t n) {
    void *p = arena_alloc(&env->value_perm, n);
    if (n) memset(p, 0, n);
    return p;
}

char *turi_val_perm_strdup(TuriEnv *env, const char *s) {
    if (!s) return NULL;
    return arena_strdup(&env->value_perm, s, strlen(s));
}

/* Process-global fallback pool for the env-less error/rejection constructors.
 * Lazily initialised on first use; reclaimed by turi_val_global_pool_free,
 * which turi_env_free calls (single-env embedding pattern). */
static Arena g_turi_val_pool;
static bool  g_turi_val_pool_init = false;

char *turi_val_strdup_global(const char *s) {
    if (!s) return NULL;
    if (!g_turi_val_pool_init) {
        arena_init(&g_turi_val_pool, 0);
        g_turi_val_pool_init = true;
    }
    return arena_strdup(&g_turi_val_pool, s, strlen(s));
}

void turi_val_global_pool_free(void) {
    if (g_turi_val_pool_init) {
        arena_free(&g_turi_val_pool);
        g_turi_val_pool_init = false;
    }
}

TuriValue turi_error(const char *msg) {
    return (TuriValue){TURI_ERROR, .as_error = turi_val_strdup_global(msg)};
}

TuriValue turi_errorf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return (TuriValue){TURI_ERROR, .as_error = turi_val_strdup_global(buf)};
}

TuriValue turi_rejection(const char *msg) {
    return (TuriValue){TURI_REJECTION, .as_error = turi_val_strdup_global(msg ? msg : "")};
}

void turi_print_value(FILE *out, TuriValue v) {
    switch (v.tag) {
    case TURI_NIL:
        fprintf(out, "nil");
        break;
    case TURI_BOOL:
        fprintf(out, "%s", v.as_bool ? "true" : "false");
        break;
    case TURI_INT:
        fprintf(out, "%lld", (long long)v.as_int);
        break;
    case TURI_FLOAT:
        fprintf(out, "%g", v.as_float);
        break;
    case TURI_CSTR:
        fprintf(out, "\"%s\"", v.as_cstr ? v.as_cstr : "");
        break;
    case TURI_CLOSURE: {
        /* Print as #<fn name> if we can access the function name */
        fprintf(out, "#<fn>");
        break;
    }
    case TURI_ERROR:
        fprintf(out, "#<error: %s>", v.as_error ? v.as_error : "");
        break;
    case TURI_EFFECT_CONT:
        fprintf(out, "#<continuation>");
        break;
    case TURI_STRUCT:
        fprintf(out, "#<struct>");
        break;
    case TURI_THROW:
        fprintf(out, "#<exception>");
        break;
    case TURI_FUTURE:
        fprintf(out, "#<future>");
        break;
    case TURI_REF:
        fprintf(out, "#<ref>");
        break;
    case TURI_STRUCT_TYPE:
        fprintf(out, "#<struct-type %s>", v.as_cstr ? v.as_cstr : "?");
        break;
    case TURI_GEN:
        fprintf(out, "#<generator>");
        break;
    case TURI_HANDLER:
        fprintf(out, "#<handler>");
        break;
    case TURI_REJECTION:
        fprintf(out, "#<rejection: %s>", v.as_error ? v.as_error : "");
        break;
    }
}
