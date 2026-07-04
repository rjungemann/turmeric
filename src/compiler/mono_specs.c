/* mono_specs.c -- VBM1/VBM2 by-value HKT monomorphization spec registry.
 *
 * See mono_specs.h and docs/upcoming/van-laarhoven-monomorphization-plan.md.
 *
 * VBM1 populates a flat, deduped table of ABSTRACT lens specialization keys
 * during elaboration (each keyed on the abstract lens param `l` at the `(l g s)`
 * pin inside the enclosing fn).  VBM2 (`mono_specs_resolve_program`) walks the
 * elaborated program and resolves each abstract key to the CONCRETE lens defn
 * passed at every top-level call of its enclosing fn, recording a concrete key.
 * `--dump-mono-specs` prints both tables for review before VBM2b wires the
 * per-spec by-value body emit. */
#include "mono_specs.h"
#include "expr.h"

#include <stdint.h>
#include <string.h>

#define MONO_SPEC_FIELD 96
#define MONO_SPEC_MAX   1024

/* An ABSTRACT spec key registered by VBM1 at the `(l g s)` pin. */
typedef struct {
    char     enclosing[MONO_SPEC_FIELD];
    char     callee[MONO_SPEC_FIELD];    /* the abstract lens param name (`l`) */
    char     functor[MONO_SPEC_FIELD];
    char     focus[MONO_SPEC_FIELD];
    char     whole[MONO_SPEC_FIELD];
    char     tyvar[MONO_SPEC_FIELD];     /* the HKT constraint var name (`f`) */
    Type     functor_ty;                 /* the resolved concrete functor ctor */
    bool     have_functor_ty;
    uint64_t hash;
} MonoSpecKey;

static MonoSpecKey g_specs[MONO_SPEC_MAX];
static size_t      g_n_specs = 0;

/* A CONCRETE spec key resolved by VBM2: the abstract lens param collapsed to the
 * concrete lens defn actually passed at a top-level invocation. */
typedef struct {
    char        lens[MONO_SPEC_FIELD];   /* concrete lens defn name (`point-x`) */
    char        functor[MONO_SPEC_FIELD];
    char        focus[MONO_SPEC_FIELD];
    char        whole[MONO_SPEC_FIELD];
    char        tyvar[MONO_SPEC_FIELD];
    Type        functor_ty;
    bool        have_functor_ty;
    const void *lens_fn;                 /* resolved `const FnDef *` for the lens */
    uint64_t    hash;
} MonoConcreteKey;

static MonoConcreteKey g_concrete[MONO_SPEC_MAX];
static size_t          g_n_concrete = 0;

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
                        const char *whole_ty, const char *tyvar,
                        const void *functor_ty) {
    if (g_n_specs >= MONO_SPEC_MAX) return;
    MonoSpecKey k;
    memset(&k, 0, sizeof k);
    field_copy(k.enclosing, enclosing_fn);
    field_copy(k.callee, callee);
    field_copy(k.functor, functor_name);
    field_copy(k.focus, focus_ty);
    field_copy(k.whole, whole_ty);
    field_copy(k.tyvar, tyvar);
    if (functor_ty) { k.functor_ty = *(const Type *)functor_ty; k.have_functor_ty = true; }
    /* Content hash over the five resolved fields.  Byte-identical keys collapse
     * (a lens invoked twice from the same enclosing fn registers once).  Two
     * sites in DIFFERENT enclosing fns that pin the same (functor, focus, whole)
     * still surface separately here because the concrete lens FnDef the plan's
     * canonical key names is not resolvable at the abstract `(l g s)` pin -- the
     * cross-procedural collapse to one CONCRETE emit is VBM2's resolve pass
     * (mono_specs_resolve_program) below. */
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
size_t mono_spec_concrete_count(void) { return g_n_concrete; }

unsigned long long mono_spec_concrete_get(size_t i, const char **lens,
                                          const char **functor,
                                          const char **focus,
                                          const char **whole) {
    if (i >= g_n_concrete) return 0;
    const MonoConcreteKey *k = &g_concrete[i];
    if (lens)    *lens    = k->lens;
    if (functor) *functor = k->functor;
    if (focus)   *focus   = k->focus;
    if (whole)   *whole   = k->whole;
    return (unsigned long long)k->hash;
}

unsigned long long mono_spec_concrete_emit_info(size_t i, const void **lens_fn,
                                                const char **tyvar,
                                                const void **functor_ty) {
    if (i >= g_n_concrete) return 0;
    const MonoConcreteKey *k = &g_concrete[i];
    if (lens_fn)    *lens_fn    = k->lens_fn;
    if (tyvar)      *tyvar      = k->tyvar;
    if (functor_ty) *functor_ty = k->have_functor_ty ? &k->functor_ty : NULL;
    return (unsigned long long)k->hash;
}

/* Register (dedup by content hash) one resolved CONCRETE spec key, carrying the
 * emit handles (resolved lens FnDef + functor ctor Type + HKT tyvar name) VBM2b
 * needs to emit the specialized by-value body. */
static void mono_concrete_register(const char *lens, const MonoSpecKey *abs,
                                   const void *lens_fn) {
    if (g_n_concrete >= MONO_SPEC_MAX) return;
    MonoConcreteKey k;
    memset(&k, 0, sizeof k);
    field_copy(k.lens, lens);
    field_copy(k.functor, abs->functor);
    field_copy(k.focus, abs->focus);
    field_copy(k.whole, abs->whole);
    field_copy(k.tyvar, abs->tyvar);
    k.functor_ty = abs->functor_ty;
    k.have_functor_ty = abs->have_functor_ty;
    k.lens_fn = lens_fn;
    uint64_t h = fnv1a(k.lens);
    h = h * 1099511628211ULL + fnv1a(k.functor);
    h = h * 1099511628211ULL + fnv1a(k.focus);
    h = h * 1099511628211ULL + fnv1a(k.whole);
    k.hash = h;
    for (size_t i = 0; i < g_n_concrete; i++)
        if (g_concrete[i].hash == h) return; /* dedup */
    g_concrete[g_n_concrete++] = k;
}

/* ------------------------------------------------------------------ *
 * VBM2 resolution pass: join abstract lens specs to concrete lenses.  *
 * ------------------------------------------------------------------ */

/* Peel type-erasing / fat-conversion wrappers off a call argument and, if it
 * bottoms out at a named variable (a global lens defn passed by name), return
 * that name.  NULL for anonymous `(fn ...)` lenses and non-var args. */
static const char *arg_global_fn_name(const Expr *arg) {
    while (arg) {
        if (arg->kind == EX_ASCRIBE)          arg = arg->as.ascribe_.inner;
        else if (arg->kind == EX_FN_TO_FAT)   arg = arg->as.fn_to_fat_.inner;
        else if (arg->kind == EX_POLY_TO_FAT) arg = arg->as.poly_to_fat_.inner;
        else if (arg->kind == EX_POLY_WRAP)   arg = arg->as.poly_wrap_.inner;
        else break;
    }
    if (arg && arg->kind == EX_VAR && arg->as.var.binding &&
        arg->as.var.binding->name && arg->as.var.binding->name->name)
        return arg->as.var.binding->name->name;
    return NULL;
}

static const FnDef *find_fndef(const Expr *prog, const char *name);

typedef struct {
    const char        *enclosing; /* enclosing fn name whose calls we resolve */
    uint32_t           lens_idx;  /* arg slot holding the concrete lens */
    const MonoSpecKey *abs;       /* the abstract key being resolved */
    const Expr        *prog;      /* program root, for lens-name -> FnDef */
} ResolveCtx;

/* Recursively visit `e`, registering a concrete spec for every EX_CALL of
 * `rc->enclosing` whose lens-slot arg resolves to a named lens defn. */
static void resolve_walk(const Expr *e, const ResolveCtx *rc) {
    if (!e) return;
    if (e->kind == EX_CALL) {
        const Binding *fb = e->as.call_.fn_binding;
        if (fb && fb->name && fb->name->name &&
            strcmp(fb->name->name, rc->enclosing) == 0 &&
            rc->lens_idx < e->as.call_.n_args) {
            const char *lens = arg_global_fn_name(e->as.call_.args[rc->lens_idx]);
            if (lens)
                mono_concrete_register(lens, rc->abs,
                                       find_fndef(rc->prog, lens));
        }
    }
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                resolve_walk(e->as.program.items[i], rc);
            break;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn) resolve_walk(e->as.fn_def_.fn->body, rc);
            break;
        case EX_FN:
            if (e->as.fn_.fn) resolve_walk(e->as.fn_.fn->body, rc);
            break;
        case EX_CLOSURE:
            if (e->as.closure_.closure && e->as.closure_.closure->fn)
                resolve_walk(e->as.closure_.closure->fn->body, rc);
            break;
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                resolve_walk(e->as.let_.bindings[i].init, rc);
            resolve_walk(e->as.let_.body, rc);
            break;
        case EX_IF:
            resolve_walk(e->as.if_.cond, rc);
            resolve_walk(e->as.if_.then_, rc);
            resolve_walk(e->as.if_.else_or_null, rc);
            break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                resolve_walk(e->as.do_.items[i], rc);
            break;
        case EX_WHILE:
            resolve_walk(e->as.while_.cond, rc);
            resolve_walk(e->as.while_.body, rc);
            break;
        case EX_SET:
            resolve_walk(e->as.set_.value, rc);
            break;
        case EX_DEF:
            resolve_walk(e->as.def_.init, rc);
            break;
        case EX_CALL:
            resolve_walk(e->as.call_.fn_expr, rc);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                resolve_walk(e->as.call_.args[i], rc);
            break;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                resolve_walk(e->as.builtin.args[i], rc);
            break;
        case EX_RETURN:
            resolve_walk(e->as.return_.value, rc);
            break;
        case EX_ASCRIBE:
            resolve_walk(e->as.ascribe_.inner, rc);
            break;
        case EX_FN_TO_FAT:
            resolve_walk(e->as.fn_to_fat_.inner, rc);
            break;
        case EX_POLY_TO_FAT:
            resolve_walk(e->as.poly_to_fat_.inner, rc);
            break;
        case EX_POLY_WRAP:
            resolve_walk(e->as.poly_wrap_.inner, rc);
            break;
        case EX_MATCH:
            resolve_walk(e->as.match_.scrutinee, rc);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                resolve_walk(e->as.match_.arms[i].body, rc);
                resolve_walk(e->as.match_.arms[i].guard, rc);
            }
            break;
        default:
            break;
    }
}

/* Find the top-level FnDef named `name` in the elaborated program. */
static const FnDef *find_fndef(const Expr *prog, const char *name) {
    if (!prog || prog->kind != EX_PROGRAM || !name) return NULL;
    for (uint32_t i = 0; i < prog->as.program.n; i++) {
        const Expr *it = prog->as.program.items[i];
        if (it && it->kind == EX_FN_DEF && it->as.fn_def_.fn &&
            it->as.fn_def_.fn->binding && it->as.fn_def_.fn->binding->name &&
            it->as.fn_def_.fn->binding->name->name &&
            strcmp(it->as.fn_def_.fn->binding->name->name, name) == 0)
            return it->as.fn_def_.fn;
    }
    return NULL;
}

void mono_specs_resolve_program(const void *prog_) {
    const Expr *prog = (const Expr *)prog_;
    if (!prog || prog->kind != EX_PROGRAM || g_n_specs == 0) return;
    for (size_t i = 0; i < g_n_specs; i++) {
        const MonoSpecKey *k = &g_specs[i];
        /* The abstract lens param lives on the enclosing fn; find its slot. */
        const FnDef *enc = find_fndef(prog, k->enclosing);
        if (!enc) continue;
        int lens_idx = -1;
        for (uint8_t p = 0; p < enc->n_params; p++) {
            const Binding *pb = enc->params ? enc->params[p] : NULL;
            if (pb && pb->name && pb->name->name &&
                strcmp(pb->name->name, k->callee) == 0) {
                lens_idx = p;
                break;
            }
        }
        if (lens_idx < 0) continue;
        ResolveCtx rc = { k->enclosing, (uint32_t)lens_idx, k, prog };
        resolve_walk(prog, &rc);
    }
}

void mono_specs_dump(FILE *out) {
    if (!out) return;
    fprintf(out,
            "; van-laarhoven by-value monomorphization specs: %zu abstract, "
            "%zu concrete\n",
            g_n_specs, g_n_concrete);
    for (size_t i = 0; i < g_n_specs; i++) {
        const MonoSpecKey *k = &g_specs[i];
        fprintf(out,
                "mono-spec-abstract %016llx fn=%s lens-param=%s f=%s focus=%s "
                "whole=%s\n",
                (unsigned long long)k->hash, k->enclosing, k->callee,
                k->functor, k->focus, k->whole);
    }
    for (size_t i = 0; i < g_n_concrete; i++) {
        const MonoConcreteKey *k = &g_concrete[i];
        fprintf(out,
                "mono-spec %016llx lens=%s f=%s focus=%s whole=%s\n",
                (unsigned long long)k->hash, k->lens, k->functor, k->focus,
                k->whole);
    }
}

void mono_specs_reset(void) { g_n_specs = 0; g_n_concrete = 0; }
