/* mono_specs.c -- VBM1 by-value HKT monomorphization spec registry.
 *
 * See mono_specs.h and docs/upcoming/van-laarhoven-monomorphization-plan.md.
 * A flat, deduped table of lens specialization keys discovered during
 * elaboration; dumped by `--dump-mono-specs` for review before VBM2 wires the
 * per-spec body emit. */
#include "mono_specs.h"

#include <stdint.h>
#include <string.h>

#define MONO_SPEC_FIELD 96
#define MONO_SPEC_MAX   1024

typedef struct {
    char     enclosing[MONO_SPEC_FIELD];
    char     callee[MONO_SPEC_FIELD];
    char     functor[MONO_SPEC_FIELD];
    char     focus[MONO_SPEC_FIELD];
    char     whole[MONO_SPEC_FIELD];
    uint64_t hash;
} MonoSpecKey;

static MonoSpecKey g_specs[MONO_SPEC_MAX];
static size_t      g_n_specs = 0;

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; s && *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

/* Copy `src` (default "?") into a fixed field, truncating to fit. */
static void field_copy(char *dst, const char *src) {
    if (!src || !*src) src = "?";
    size_t n = strlen(src);
    if (n >= MONO_SPEC_FIELD) n = MONO_SPEC_FIELD - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void mono_spec_register(const char *enclosing_fn, const char *callee,
                        const char *functor_name, const char *focus_ty,
                        const char *whole_ty) {
    if (g_n_specs >= MONO_SPEC_MAX) return;
    MonoSpecKey k;
    memset(&k, 0, sizeof k);
    field_copy(k.enclosing, enclosing_fn);
    field_copy(k.callee, callee);
    field_copy(k.functor, functor_name);
    field_copy(k.focus, focus_ty);
    field_copy(k.whole, whole_ty);
    /* Content hash over the five resolved fields.  Byte-identical keys collapse
     * (a lens invoked twice from the same enclosing fn registers once).  Two
     * sites in DIFFERENT enclosing fns that pin the same (functor, focus, whole)
     * still surface separately here because the concrete lens FnDef the plan's
     * canonical key names is not resolvable at the abstract `(l g s)` pin -- the
     * cross-procedural collapse to one emit is plan OQ #1/#2, deferred to VBM2. */
    uint64_t h = fnv1a(k.enclosing);
    h = h * 1099511628211ULL + fnv1a(k.callee);
    h = h * 1099511628211ULL + fnv1a(k.functor);
    h = h * 1099511628211ULL + fnv1a(k.focus);
    h = h * 1099511628211ULL + fnv1a(k.whole);
    k.hash = h;
    for (size_t i = 0; i < g_n_specs; i++)
        if (g_specs[i].hash == h) return; /* dedup */
    g_specs[g_n_specs++] = k;
}

size_t mono_spec_count(void) { return g_n_specs; }

void mono_specs_dump(FILE *out) {
    if (!out) return;
    fprintf(out,
            "; van-laarhoven by-value monomorphization specs: %zu\n",
            g_n_specs);
    for (size_t i = 0; i < g_n_specs; i++) {
        const MonoSpecKey *k = &g_specs[i];
        fprintf(out,
                "mono-spec %016llx fn=%s callee=%s f=%s focus=%s whole=%s\n",
                (unsigned long long)k->hash, k->enclosing, k->callee,
                k->functor, k->focus, k->whole);
    }
}

void mono_specs_reset(void) { g_n_specs = 0; }
