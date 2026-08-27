/* interpreter_natives.c -- interpreter native overrides for stdlib inline-C ops.
 *
 * Relocated verbatim from src/main.c (the ~3900-line wk_register_* / native_*
 * block) so it lands in tur_core / libturi and is available to every
 * interpreter entry point -- not just the `tur` CLI.  The two entry points that
 * do not link main.c (the WASM REPL and the interactive `tur repl`) previously
 * could not evaluate any op whose body resolved to one of these natives.  See
 * interpreter_natives.h and docs/archive/history/web-repl-repl-inline-c-native-gap.md.
 *
 * The fixture-runner helpers (wk_apply_flags / wk_write_result / wk_drain_pipes
 * / wk_eval_fixture) that fork/pipe stay in src/main.c; only the native
 * overrides moved here.
 */
#include "turi/interpreter_natives.h"

#include "turi/eval.h"
#include "turi/collections_native.h"  /* native_mk_cmp_int / native_mk_box_cstr */
#include "diag.h"    /* SYNTAX natives: diag file-registry save/restore for read-string */
#include "forms.h"   /* SYNTAX natives: Form constructors/accessors */
#include "reader.h"  /* SYNTAX natives: read_all_with_registry for read-string */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../runtime/tur_string.h"
#if defined(_WIN32)
/* Windows has no fork/exec.  The CRT's _spawnvp/_cwait are the direct
 * equivalents (spawn-and-return-a-handle, then reap it), so process/spawn and
 * process/wait are really implemented here rather than stubbed. */
#include <process.h>
#elif !defined(__EMSCRIPTEN__)
/* <sys/wait.h> transitively pulls <signal.h>, whose emscripten build defines
 * its own ucontext_t -- colliding with the WASM fiber-stub ucontext_t in
 * turi/env.h. Process spawn/wait is a no-op under emscripten (no fork/exec in
 * the browser), so the header is only needed off the WASM build. */
#include <sys/wait.h>
#endif


/* -------------------------------------------------------------------------
 * Native implementations of common inline-C stdlib patterns.
 * Registered under their Turmeric function names; EX_FN_DEF preservation
 * keeps them when fixtures define the same function with inline-C body.
 *
 * Option struct layout: { bool is_some (offset 0); int64_t value (offset 8) }
 * Matching C struct { bool; int64_t } with 7-byte padding.
 * We represent this as int64_t[2]: [0]=is_some flag, [1]=value.
 * ---------------------------------------------------------------------- */

/* Option functions */
/* SR2b: stdlib Option/Result are real sums, and the tree-walker represents an
 * ADT constructor value as a TuriStruct NAMED BY THE CONSTRUCTOR ("Some" with
 * one payload field, "None" with none; "Ok"/"Err" with one payload field) --
 * that is what the EX_MATCH ctor patterns compare against, so the natives
 * must build exactly this shape or every stdlib `(match o (Some v) ...)`
 * body dies with "no arm matched".  The field readers below keep the legacy
 * representations readable (the int64[2]/[3] carrier boxes hand-rolled
 * inline-C still produces, and the pre-sum record TuriStructs) so mixed
 * flows keep working. */
static bool ctor_struct_is(TuriValue v, const char *name) {
    if (v.tag != TURI_STRUCT) return false;
    const char *sn = turi_struct_name(v);
    return sn && strcmp(sn, name) == 0;
}
static TuriValue ctor_struct_payload(TuriValue v) {
    bool found = false;
    TuriValue f = turi_struct_field(v, 0, &found);
    return found ? f : turi_int(0);
}
static TuriValue native_some(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    TuriValue payload = (n > 0) ? a[0] : turi_int(0);
    TuriValue fields[1] = { payload };
    return turi_make_struct(env, "Some", fields, 1);
}
static TuriValue native_none(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)a; (void)n; (void)ud;
    return turi_make_struct(env, "None", NULL, 0);
}
/* W1b + SR2b: an Option reaches these shims as a ctor-named TuriStruct
 * ("Some"/"None", from native_some/none or an interpreted `(Some x)`), as a
 * native int64[2] box {is_some, value} (hand-rolled inline-C / tur_box_some),
 * or as a legacy make-struct TuriStruct with {is_some, value} field order.
 * option_field maps the ctor rep onto the legacy field indices (0 = is_some,
 * 1 = value) so every reader keeps a single call site.  none is the "None"
 * struct or the 0/NULL box (every field 0). */
static TuriValue option_field(TuriValue o, int idx) {
    if (ctor_struct_is(o, "Some"))
        return idx == 0 ? turi_bool(true) : ctor_struct_payload(o);
    if (ctor_struct_is(o, "None"))
        return idx == 0 ? turi_bool(false) : turi_int(0);
    bool found = false;
    TuriValue f = turi_struct_field(o, (uint32_t)idx, &found);
    if (found) return f;
    int64_t *p = (int64_t *)(intptr_t)o.as_int;
    return turi_int(p ? p[idx] : 0);
}
static int64_t option_field_int(TuriValue o, int idx) {
    TuriValue f = option_field(o, idx);
    return (f.tag == TURI_BOOL) ? (f.as_bool ? 1 : 0) : f.as_int;
}
static bool option_is_some(TuriValue o) {
    if (o.tag != TURI_STRUCT && o.as_int == 0) return false;  /* none */
    return option_field_int(o, 0) != 0;
}
/* option-eq? -- structural Option equality.  option.tur's body fat-dispatches
 * the value comparator through a C function pointer; this native re-implements
 * it over either Option representation (option_field) and invokes the comparator
 * (a turi closure) via turi_call. */
static TuriValue native_option_eq(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_bool(false);
    bool a_some = option_is_some(a[0]);
    bool b_some = option_is_some(a[1]);
    if (!a_some && !b_some) return turi_bool(true);
    if (a_some != b_some)   return turi_bool(false);
    TuriValue cargs[2];
    cargs[0] = option_field(a[0], 1);
    cargs[1] = option_field(a[1], 1);
    TuriValue rv = turi_call(env, a[2], cargs, 2);
    if (turi_is_error(rv) || env->throwing) return rv;  /* propagate callback error */
    return turi_bool(rv.tag == TURI_BOOL ? rv.as_bool : rv.as_int != 0);
}
static TuriValue native_some_pred(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_bool(false);
    return turi_bool(option_is_some(a[0]));
}
static TuriValue native_option_unwrap(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    if (!option_is_some(a[0])) { fprintf(stderr, "unwrap called on none\n"); return turi_int(0); }
    /* SR2b: hand back the FULL payload value (tag preserved) -- flattening to
     * int lost float/cstr/struct payloads. */
    return option_field(a[0], 1);
}
static TuriValue native_option_value(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    return native_option_unwrap(env, a, n, ud);
}
static TuriValue native_option_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    /* A ctor-named TuriStruct is env-pool-owned -- only the legacy raw box is
     * individually freed. */
    if (n > 0 && a[0].tag != TURI_STRUCT) {
        void *p = (void *)(intptr_t)a[0].as_int; if (p) free(p);
    }
    return turi_nil();
}
static TuriValue native_option_must(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1 || !option_is_some(a[0])) {
        /* Catchable panic (recoverable by catch-unwind) instead of _exit(1). */
        turi_runtime_panic(env, "option-must: called on none");
        return turi_nil(); /* unreachable */
    }
    return option_field(a[0], 1);
}
static TuriValue native_option_expect(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    const char *msg = (n > 1 && a[1].tag == TURI_CSTR && a[1].as_cstr) ? a[1].as_cstr : "option-expect: called on none";
    if (n < 1 || !option_is_some(a[0])) {
        turi_runtime_panic(env, msg);
        return turi_nil(); /* unreachable */
    }
    return option_field(a[0], 1);
}
static TuriValue native_option_unwrap_or(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    if (!option_is_some(a[0])) return a[1];
    return option_field(a[0], 1);
}
/* option-map -- apply f to the some value, returning a new Option.  option.tur's
 * body fat-dispatches f via TUR_APPLY1; this native invokes f (a turi closure)
 * via turi_call and returns a fresh native int64[2] {is_some, value} box. */
static TuriValue native_option_map(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_make_struct(env, "None", NULL, 0);
    if (!option_is_some(a[0])) return turi_make_struct(env, "None", NULL, 0);
    TuriValue arg = option_field(a[0], 1);
    TuriValue rv = turi_call(env, a[1], &arg, 1);
    if (turi_is_error(rv) || env->throwing) return rv;  /* propagate callback error */
    TuriValue fields[1] = { rv };
    return turi_make_struct(env, "Some", fields, 1);
}

/* Result functions: { bool is_ok (offset 0); int64_t ok_val (offset 8); int64_t err_val (offset 16) }
 * Stored as int64_t[3]: [0]=is_ok, [1]=ok_val, [2]=err_val
 *
 * R5 (turi-interpret-flip-residual-plan): the int64 box flattens the payload to
 * a bare int64, which loses the tag of a *heap* payload (a make-struct User, a
 * cstr, a closure) -- ok-val then hands back a TURI_INT and a downstream field
 * access / println reads garbage (the value-struct-payload silent miscompile).
 * So when the payload is a heap value, build a make-struct Result instead, whose
 * fields hold the full TuriValue (tag preserved); int/bool payloads keep the box
 * (no change to the carrier-ABI fixtures that depend on it). result_field reads
 * both reps, so ok?/err?/ok-val/err-val/result-eq stay uniform. */
static TuriValue native_ok(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    TuriValue payload = (n > 0) ? a[0] : turi_int(0);
    TuriValue fields[1] = { payload };
    return turi_make_struct(env, "Ok", fields, 1);
}
static TuriValue native_err(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    TuriValue payload = (n > 0) ? a[0] : turi_int(0);
    TuriValue fields[1] = { payload };
    return turi_make_struct(env, "Err", fields, 1);
}
/* W1b: a Result reaches these shims either as a native int64[3] box
 * {is_ok, ok_val, err_val} (from native_ok/err) or as a make-struct TuriStruct
 * with the same field order (from `(make-struct Result ...)`).  These helpers
 * read field `idx` from whichever representation so the two coexist -- one of
 * the three pieces (with native_result_eq and the EX_GET_FIELD carrier path)
 * that let result.tur join the prelude. */
static TuriValue result_field(TuriValue r, int idx) {
    /* SR2b: ctor-named rep first (see the Option twin above): "Ok"/"Err"
     * structs carry one payload field; map onto the legacy indices
     * (0 = is_ok, 1 = ok_val, 2 = err_val). */
    if (ctor_struct_is(r, "Ok"))
        return idx == 0 ? turi_bool(true)
             : idx == 1 ? ctor_struct_payload(r) : turi_int(0);
    if (ctor_struct_is(r, "Err"))
        return idx == 0 ? turi_bool(false)
             : idx == 2 ? ctor_struct_payload(r) : turi_int(0);
    bool found = false;
    TuriValue f = turi_struct_field(r, (uint32_t)idx, &found);
    if (found) return f;
    int64_t *p = (int64_t *)(intptr_t)r.as_int;
    return turi_int(p ? p[idx] : 0);
}
static int64_t result_field_int(TuriValue r, int idx) {
    TuriValue f = result_field(r, idx);
    return (f.tag == TURI_BOOL) ? (f.as_bool ? 1 : 0) : f.as_int;
}
/* True if the Result carries the ok tag (dual-rep: TuriStruct or native box). */
static bool result_is_ok(TuriValue r) { return result_field_int(r, 0) != 0; }
/* Construct a Result from an is_ok flag + payload, dual-rep-correct: a heap
 * payload becomes a make-struct Result, a scalar a native int64[3] box -- exactly
 * native_ok/native_err's logic, so the reading shims (result_field) round-trip it
 * and a struct ok/err value keeps its tag instead of being flattened to int. */
static TuriValue result_box_make(TuriEnv *env, bool is_ok, TuriValue payload) {
    TuriValue arg[1] = { payload };
    return is_ok ? native_ok(env, arg, 1, NULL) : native_err(env, arg, 1, NULL);
}
static TuriValue native_ok_pred(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_bool(false);
    if (a[0].tag != TURI_STRUCT && a[0].as_int == 0) return turi_bool(false);
    return turi_bool(result_field_int(a[0], 0) != 0);
}
static TuriValue native_err_pred(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_bool(true);
    if (a[0].tag != TURI_STRUCT && a[0].as_int == 0) return turi_bool(true);
    return turi_bool(result_field_int(a[0], 0) == 0);
}
static TuriValue native_ok_val(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    return result_field(a[0], 1);
}
static TuriValue native_err_val(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    return result_field(a[0], 2);
}
static TuriValue native_result_unwrap_or(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    if (result_is_ok(a[0])) return result_field(a[0], 1);
    return a[1];
}
/* ok-val-ptr: get ok_val as a pointer (void*) */
static TuriValue native_ok_val_ptr(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    return result_field(a[0], 1);
}
/* err-val-ptr: get err_val as a pointer (void*) */
static TuriValue native_err_val_ptr(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    return result_field(a[0], 2);
}
/* result-map: apply Turmeric fn to ok value, return new result */
static TuriValue native_result_map(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    if (!result_is_ok(a[0])) return a[0]; /* return err unchanged */
    TuriValue arg = result_field(a[0], 1);
    TuriValue res = turi_call(env, a[1], &arg, 1);
    if (turi_is_error(res) || env->throwing) return res;  /* propagate callback error */
    return result_box_make(env, true, res);
}
/* W1b: result-eq? -- (result-eq? r1 r2 ok-cmp err-cmp).  result.tur's version is
 * inline-C that fat-dispatches the two comparison closures, which the simple
 * inline-C evaluator cannot run; this native invokes them via turi_call so
 * typed/result-basic and friends work under --interpret.  Reads both Results
 * dual-rep (make-struct TuriStruct or native box) via result_field. */
static TuriValue native_result_eq(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return turi_bool(false);
    int64_t a_ok = result_field_int(a[0], 0);
    int64_t b_ok = result_field_int(a[1], 0);
    if ((a_ok != 0) != (b_ok != 0)) return turi_bool(false);
    int idx = a_ok ? 1 : 2;                /* compare ok-vals or err-vals */
    TuriValue cmp = a_ok ? a[2] : a[3];    /* ok-cmp or err-cmp */
    TuriValue cargs[2] = { result_field(a[0], idx), result_field(a[1], idx) };
    TuriValue res = turi_call(env, cmp, cargs, 2);
    if (turi_is_error(res) || env->throwing) return res;  /* propagate callback error */
    return turi_bool(res.tag == TURI_BOOL ? res.as_bool : res.as_int != 0);
}
/* result-map-err: apply fn to err value, return new result */
static TuriValue native_result_map_err(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    if (result_is_ok(a[0])) return a[0]; /* return ok unchanged */
    TuriValue arg = result_field(a[0], 2);
    TuriValue res = turi_call(env, a[1], &arg, 1);
    if (turi_is_error(res) || env->throwing) return res;  /* propagate callback error */
    return result_box_make(env, false, res);
}
/* result-flat-map: apply fn to ok value (fn returns a result), flatMap */
static TuriValue native_result_flat_map(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    if (!result_is_ok(a[0])) return a[0]; /* return err unchanged */
    TuriValue arg = result_field(a[0], 1);
    return turi_call(env, a[1], &arg, 1);
}
/* result-or: return self if ok, else return alt */
static TuriValue native_result_or(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_nil();
    if (result_is_ok(a[0])) return a[0];
    return a[1];
}
/* result-or-else: return self if ok, else call f(err_val) */
static TuriValue native_result_or_else(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    if (result_is_ok(a[0])) return a[0];
    TuriValue arg = result_field(a[0], 2);
    return turi_call(env, a[1], &arg, 1);
}
/* Display helpers */
static TuriValue native_result_display(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_cstr("err");
    return turi_cstr(result_is_ok(a[0]) ? "ok" : "err");
}
static TuriValue native_result_debug(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_cstr("Result::Err");
    return turi_cstr(result_is_ok(a[0]) ? "Result::Ok" : "Result::Err");
}
static TuriValue native_result_error_message(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_cstr("result is err");
    return turi_cstr(result_is_ok(a[0]) ? "result is ok" : "result is err");
}
/* result-collect: vec<result<T,E>> → result<vec<T>,E>
 * If all elements are ok, returns ok(new_vec_of_ok_vals).
 * If any element is err, returns the first err. */
static TuriValue native_result_collect(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    if (!v) return turi_nil();
    int64_t len = v[1];
    int64_t *data = (int64_t *)(intptr_t)v[0];
    /* Check for first err */
    for (int64_t i = 0; i < len; i++) {
        int64_t *rp = (int64_t *)(intptr_t)data[i];
        if (!rp || rp[0] == 0) {
            int64_t ev = rp ? rp[2] : 0;
            int64_t *out = (int64_t *)malloc(3 * sizeof(int64_t));
            if (!out) return turi_nil();
            out[0] = 0; out[1] = 0; out[2] = ev;
            TuriValue rv = {0}; rv.tag = TURI_INT; rv.as_int = (int64_t)(intptr_t)out; return rv;
        }
    }
    /* All ok: create new vec of ok_vals */
    int64_t *ov = (int64_t *)calloc(3, sizeof(int64_t));
    if (!ov) return turi_nil();
    int64_t *od = len > 0 ? (int64_t *)malloc((size_t)len * sizeof(int64_t)) : NULL;
    ov[0] = (int64_t)(intptr_t)od; ov[1] = len; ov[2] = len;
    for (int64_t i = 0; i < len; i++) {
        int64_t *rp = (int64_t *)(intptr_t)data[i];
        od[i] = rp[1]; /* ok_val */
    }
    int64_t *out = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!out) { free(od); free(ov); return turi_nil(); }
    out[0] = 1; out[1] = (int64_t)(intptr_t)ov; out[2] = 0;
    TuriValue rv = {0}; rv.tag = TURI_INT; rv.as_int = (int64_t)(intptr_t)out; return rv;
}
/* Pair layout: { void *ok_vec; void *err_vec; } = int64_t[2] */
static TuriValue native_result_partition(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    if (!v) return turi_nil();
    int64_t len = v[1];
    int64_t *data = (int64_t *)(intptr_t)v[0];
    /* Allocate ok and err vecs */
    int64_t *ov = (int64_t *)calloc(3, sizeof(int64_t));
    int64_t *ev = (int64_t *)calloc(3, sizeof(int64_t));
    if (!ov || !ev) { free(ov); free(ev); return turi_nil(); }
    for (int64_t i = 0; i < len; i++) {
        int64_t *rp = (int64_t *)(intptr_t)data[i];
        int64_t *dst = (rp && rp[0] != 0) ? ov : ev;
        int64_t val = (rp && rp[0] != 0) ? rp[1] : (rp ? rp[2] : 0);
        /* Push to dst vec */
        int64_t dlen = dst[1], dcap = dst[2];
        int64_t *ddata = (int64_t *)(intptr_t)dst[0];
        if (dlen >= dcap) {
            int64_t nc = dcap > 0 ? dcap * 2 : 4;
            int64_t *nd = (int64_t *)malloc((size_t)nc * sizeof(int64_t));
            if (!nd) continue;
            for (int64_t j = 0; j < dlen; j++) nd[j] = ddata[j];
            free(ddata); dst[0] = (int64_t)(intptr_t)nd; dst[2] = nc; ddata = nd;
        }
        ddata[dlen] = val; dst[1] = dlen + 1;
    }
    int64_t *pair = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!pair) { return turi_nil(); }
    pair[0] = (int64_t)(intptr_t)ov; pair[1] = (int64_t)(intptr_t)ev;
    TuriValue rv = {0}; rv.tag = TURI_INT; rv.as_int = (int64_t)(intptr_t)pair; return rv;
}
static TuriValue native_result_partition_ok(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *pair = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = pair ? pair[0] : 0; return v;
}
static TuriValue native_result_partition_err(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *pair = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = pair ? pair[1] : 0; return v;
}
static TuriValue native_result_unwrap(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    if (!result_is_ok(a[0])) { fprintf(stderr, "unwrap called on err\n"); return turi_int(0); }
    return result_field(a[0], 1);
}
static TuriValue native_result_unwrap_err(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    return result_field(a[0], 2);
}
static TuriValue native_result_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].tag != TURI_STRUCT) {
        void *p = (void *)(intptr_t)a[0].as_int; if (p) free(p);
    }
    return turi_nil();
}
static TuriValue native_result_must(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1 || !result_is_ok(a[0])) {
        /* Catchable panic (recoverable by catch-unwind) instead of _exit(1). */
        turi_runtime_panic(env, "result-must: called on err");
        return turi_nil(); /* unreachable */
    }
    return result_field(a[0], 1);
}
static TuriValue native_result_must_msg(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    const char *msg = (n > 1 && a[1].tag == TURI_CSTR && a[1].as_cstr) ? a[1].as_cstr : "result-must-msg: called on err";
    if (n < 1 || !result_is_ok(a[0])) {
        turi_runtime_panic(env, msg);
        return turi_nil(); /* unreachable */
    }
    return result_field(a[0], 1);
}

/* int->str / show-int-as-cstr: format int64 as decimal string */
static TuriValue native_int_to_str(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = (n > 0) ? a[0].as_int : 0;
    char *buf = (char *)malloc(32);
    if (!buf) return turi_nil();
    snprintf(buf, 32, "%lld", (long long)v);
    return turi_cstr(buf);
}

/* str-concat: mirror stdlib/str.tur:99 -- allocate la+lb+1, copy both halves,
 * NUL-terminate.  Layout-exact with the compiled cstr ABI (a NUL-terminated
 * char* boxed as a cstr value), so a value crossing between interpreted and
 * native code reads identically.  Allocated from the env value pool, not
 * malloc: nothing ever frees the result individually (by design), and a
 * malloc here is a REAL leak when the native runs at macro-expansion time
 * inside the leak-checked `tur build`/`emit-c` path (defmacro* bodies). */
static TuriValue native_str_concat(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    const char *sa = (n > 0 && a[0].tag == TURI_CSTR && a[0].as_cstr) ? a[0].as_cstr : "";
    const char *sb = (n > 1 && a[1].tag == TURI_CSTR && a[1].as_cstr) ? a[1].as_cstr : "";
    size_t la = strlen(sa), lb = strlen(sb);
    char *out = (char *)turi_val_alloc(env, la + lb + 1);
    if (!out) return turi_nil();
    memcpy(out, sa, la);
    memcpy(out + la, sb, lb);
    out[la + lb] = '\0';
    return turi_cstr(out);
}

/* cstr-len: mirror stdlib/cstr.tur:26 -- byte length of a NUL-terminated cstr. */
static TuriValue native_cstr_len(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *s = (n > 0 && a[0].tag == TURI_CSTR && a[0].as_cstr) ? a[0].as_cstr : "";
    return turi_int((int64_t)strlen(s));
}

/* cstr-nth: mirror stdlib/cstr.tur:41 -- unsigned byte at index i.  Caller
 * ensures 0 <= i < (cstr-len s), matching the compiled contract. */
static TuriValue native_cstr_nth(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *s = (n > 0 && a[0].tag == TURI_CSTR && a[0].as_cstr) ? a[0].as_cstr : "";
    int64_t i = (n > 1) ? a[1].as_int : 0;
    return turi_int((int64_t)(unsigned char)s[i]);
}

/* str->int: parse decimal string to int64 */
static TuriValue native_str_to_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || a[0].tag != TURI_CSTR || !a[0].as_cstr) return turi_int(0);
    return turi_int((int64_t)atoll(a[0].as_cstr));
}

/* strcmp: compare two cstr values */
static TuriValue native_strcmp_fn(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *s1 = (n > 0 && a[0].tag == TURI_CSTR) ? a[0].as_cstr : "";
    const char *s2 = (n > 1 && a[1].tag == TURI_CSTR) ? a[1].as_cstr : "";
    return turi_int((int64_t)strcmp(s1, s2));
}

/* int-val: dereference an int64_t* pointer and return the stored int.
 * Some fixture tests call this after free (same pattern as compiled C).
 * Suppress ASAN to match compiled-mode behavior. */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
__attribute__((no_sanitize("address")))
#  endif
#elif defined(__SANITIZE_ADDRESS__)
__attribute__((no_sanitize("address")))
#endif
static TuriValue native_int_val(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *p = (int64_t *)(intptr_t)a[0].as_int;
    if (!p) return turi_int(0);
    return turi_int(*p);
}
/* cstr-sub: substring [start,end) of a cstr as a fresh NUL-terminated buffer.
 * Mirrors stdlib/cstr.tur's cstr-sub. Clamps start/end into [0,len] and
 * start<=end so a caller cannot walk off the buffer. Returns TURI_CSTR. */
static TuriValue native_cstr_sub(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *s = (n > 0 && a[0].tag == TURI_CSTR && a[0].as_cstr)
                      ? a[0].as_cstr : (const char *)(intptr_t)(n > 0 ? a[0].as_int : 0);
    if (!s) s = "";
    int64_t slen = (int64_t)strlen(s);
    int64_t start = (n > 1) ? a[1].as_int : 0;
    int64_t end = (n > 2) ? a[2].as_int : 0;
    if (start < 0) start = 0;
    if (end > slen) end = slen;
    if (end < start) end = start;
    size_t outlen = (size_t)(end - start);
    char *out = (char *)malloc(outlen + 1);
    if (!out) return turi_nil();
    memcpy(out, s + start, outlen);
    out[outlen] = '\0';
    TuriValue v = {0}; v.tag = TURI_CSTR; v.as_cstr = out; return v;
}
/* alloc-str: strdup a cstr, return heap-allocated copy as int64_t ptr */
static TuriValue native_alloc_str(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *s = (n > 0 && a[0].tag == TURI_CSTR) ? a[0].as_cstr : "";
    char *p = strdup(s ? s : "");
    if (!p) return turi_nil();
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)p; return v;
}
/* cstr-free: free a cstr or int pointer */
static TuriValue native_cstr_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    void *p = (a[0].tag == TURI_CSTR) ? (void *)a[0].as_cstr : (void *)(intptr_t)a[0].as_int;
    if (p) free(p);
    return turi_nil();
}
/* alloc-int: malloc an int64_t cell, store x, return pointer as int */
static TuriValue native_alloc_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t *p = (int64_t *)malloc(sizeof(int64_t));
    if (!p) return turi_nil();
    *p = (n > 0) ? a[0].as_int : 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)p; return v;
}
/* ptr=: pointer equality (both stored as int64_t) */
static TuriValue native_ptr_eq(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_bool(false);
    return turi_bool(a[0].as_int == a[1].as_int);
}

/* c-abs: absolute value (fixture helper) */
static TuriValue native_c_abs(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t x = (n > 0) ? a[0].as_int : 0;
    return turi_int(x < 0 ? -x : x);
}
/* popcount: bit population count via __builtin_popcount */
static TuriValue native_popcount(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t x = (n > 0) ? a[0].as_int : 0;
    return turi_int((int64_t)__builtin_popcountll((unsigned long long)x));
}
/* flat array: calloc n int64_t cells */
static TuriValue native_flat_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t sz = (n > 0) ? a[0].as_int : 0;
    if (sz <= 0) return turi_int(0);
    int64_t *arr = (int64_t *)calloc((size_t)sz, sizeof(int64_t));
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)arr; return v;
}
/* flat-get: arr[y*width + x] */
static TuriValue native_flat_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 4) return turi_int(0);
    int64_t *arr = (int64_t *)(intptr_t)a[0].as_int;
    int64_t x = a[1].as_int, y = a[2].as_int, w = a[3].as_int;
    if (!arr) return turi_int(0);
    return turi_int(arr[y * w + x]);
}
/* flat-set: arr[y*width + x] = val, returns 0 */
static TuriValue native_flat_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 5) return turi_int(0);
    int64_t *arr = (int64_t *)(intptr_t)a[0].as_int;
    int64_t x = a[1].as_int, y = a[2].as_int, w = a[3].as_int, val = a[4].as_int;
    if (arr) arr[y * w + x] = val;
    return turi_int(0);
}

/* SEQ (stdlib/seq): the lazy-Seq inline-C bridges assume the COMPILED ABI --
 * fat-closure calls via a C function pointer and a {__state,__next_fn} generator
 * struct -- so the tree-walker cannot run them.  Under --interpret a Seq factory
 * is a TURI_CLOSURE and a generator is a TURI_GEN; re-implement the bridges over
 * turi_call + turi_gen_advance_val.  A yielded value is boxed as a malloc'd
 * int64 (NULL = exhausted), matching seq-val-some?/seq-val-unwrap. */
static TuriValue seq_as_closure(TuriValue v) {
    /* A factory/callback passed through an `int` param keeps its TURI_CLOSURE
     * tag; if it arrived as a raw carrier, rebuild the closure value. */
    if (v.tag == TURI_CLOSURE) return v;
    return turi_closure((TuriClosure *)(intptr_t)v.as_int);
}
/* k-apply-raw (stdlib/kleisli.tur): invoke the fat-closure carrier `k` on `x`,
 * returning the int-level Option it produces.  kleisli.tur's body is
 * `TUR_APPLY1(k, x)` (a compiled fat-dispatch through a C function pointer)
 * which the tree-walker cannot run; this native recovers the closure from the
 * int carrier and invokes it via turi_call.  Mirrors sch_apply1 / native_seq_*. */
static TuriValue native_k_apply_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    TuriValue av = turi_int(a[1].as_int);
    TuriValue r = turi_call(env, seq_as_closure(a[0]), &av, 1);
    if (turi_is_error(r) || env->throwing) return r;  /* propagate callback error */
    /* The arrow body returns an int-level Option.  Under --interpret the
     * Option carrier is dual-rep: `some` may yield either a native int64[2]
     * box (TURI_INT) or a make-struct TuriStruct (TURI_STRUCT).  Preserve r's
     * tag rather than flattening to turi_int(r.as_int) -- collapsing a
     * TURI_STRUCT to a bare int pointer drops the struct tag, and downstream
     * some?/unwrap-or would then misread the TuriStruct as a raw int64[2]
     * box.  Returning r keeps both representations intact for the dual-rep
     * option shims (option_field / option_is_some). */
    return r;
}
static TuriValue native_seq_iter(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1) return turi_nil();
    int64_t *s = (int64_t *)(intptr_t)a[0].as_int;
    if (!s) return turi_nil();
    return turi_call(e, seq_as_closure(turi_int(s[0])), NULL, 0); /* mk() -> gen */
}
static TuriValue native_seq_gen_done(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_bool(true);
    return turi_bool(turi_gen_done_val(a[0]));
}
static TuriValue native_seq_gen_next(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1) return turi_int(0);
    int done = 0;
    /* turi_gen_advance_val already returns a pointer to the generator's value
     * box (the compiled ptr<void> yield protocol), valid until the next advance
     * -- exactly what seq-val-some?/seq-val-unwrap expect.  Pass it through; do
     * not re-box (that would hand back a pointer-to-pointer). */
    TuriValue v = turi_gen_advance_val(e, a[0], &done);
    if (done) return turi_int(0);                       /* NULL = exhausted */
    return v;
}
static TuriValue native_seq_val_some(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_bool(n >= 1 && a[0].as_int != 0);
}
static TuriValue native_seq_val_unwrap(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(*(int64_t *)(intptr_t)a[0].as_int);
}
/* gen<->int identity casts used by the transform layer (the gen is already an
 * int64 carrier under --interpret). */
static TuriValue native_seq_gen_to_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int(n >= 1 ? a[0].as_int : 0);
}
/* Fat-closure call bridges: the callback is a TURI_CLOSURE under --interpret. */
static TuriValue native_seq_call_fn0(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1) return turi_int(0);
    return turi_call(e, seq_as_closure(a[0]), NULL, 0);
}
static TuriValue native_seq_call_fn1(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    return turi_call(e, seq_as_closure(a[0]), &a[1], 1);
}
static TuriValue native_seq_call_fn2(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_int(0);
    TuriValue args[2] = { a[1], a[2] };
    return turi_call(e, seq_as_closure(a[0]), args, 2);
}
static TuriValue native_seq_call_bool_fn1(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_bool(false);
    TuriValue r = turi_call(e, seq_as_closure(a[0]), &a[1], 1);
    if (turi_is_error(r) || e->throwing) return r;  /* propagate callback error */
    return turi_bool(r.tag == TURI_BOOL ? r.as_bool : r.as_int != 0);
}
static TuriValue native_seq_call_void_fn1(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    TuriValue r = turi_call(e, seq_as_closure(a[0]), &a[1], 1);
    if (turi_is_error(r) || e->throwing) return r;  /* propagate callback error */
    return turi_nil();
}
/* gen-arr (stdlib/gen.tur): growable int64 array {len, cap, data} used by
 * gen-collect / seq-collect.  Re-implemented natively (the inline-C grows via
 * malloc/free, which the simple executor cannot run faithfully). */
static TuriValue native_gen_arr_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    int64_t *h = (int64_t *)malloc(3 * sizeof(int64_t));
    h[0] = 0; h[1] = 0; h[2] = 0;
    return turi_int((int64_t)(intptr_t)h);
}
static TuriValue native_gen_arr_push(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    int64_t *h = (int64_t *)(intptr_t)a[0].as_int;
    if (h[0] >= h[1]) {
        int64_t nc = h[1] ? h[1] * 2 : 4;
        int64_t *nd = (int64_t *)malloc((size_t)nc * sizeof(int64_t));
        if (h[1]) {
            int64_t *od = (int64_t *)(intptr_t)h[2];
            for (int64_t k = 0; k < h[0]; k++) nd[k] = od[k];
            free(od);
        }
        h[1] = nc; h[2] = (int64_t)(intptr_t)nd;
    }
    ((int64_t *)(intptr_t)h[2])[h[0]++] = a[1].as_int;
    return turi_nil();
}
static TuriValue native_gen_arr_len(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[0]);
}
static TuriValue native_gen_arr_get(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *h = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(((int64_t *)(intptr_t)h[2])[a[1].as_int]);
}
/* seq option box {bool is_some; int64 value} (offset 0 / offset 8 = slot[1]). */
static TuriValue native_seq_make_some(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    int64_t *p = (int64_t *)malloc(2 * sizeof(int64_t));
    p[0] = 1; p[1] = (n >= 1) ? a[0].as_int : 0;
    return turi_int((int64_t)(intptr_t)p);
}
static TuriValue native_seq_make_none(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    int64_t *p = (int64_t *)malloc(2 * sizeof(int64_t));
    p[0] = 0; p[1] = 0;
    return turi_int((int64_t)(intptr_t)p);
}
static TuriValue native_seq_opt_some(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_bool(false);
    return turi_bool(((int64_t *)(intptr_t)a[0].as_int)[0] != 0);
}
static TuriValue native_seq_opt_unwrap(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[1]);
}
/* seq out-vec {int64* data; len; cap}. */
static TuriValue native_seq_out_vec_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    int64_t *v = (int64_t *)malloc(3 * sizeof(int64_t));
    v[0] = 0; v[1] = 0; v[2] = 0;
    return turi_int((int64_t)(intptr_t)v);
}
static TuriValue native_seq_out_vec_push(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;   /* {data, len, cap} */
    if (v[1] == v[2]) {
        v[2] = v[2] ? v[2] * 2 : 8;
        v[0] = (int64_t)(intptr_t)realloc((void *)(intptr_t)v[0],
                                          (size_t)v[2] * sizeof(int64_t));
    }
    ((int64_t *)(intptr_t)v[0])[v[1]++] = a[1].as_int;
    return turi_nil();
}
static TuriValue native_seq_vec_len(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[1]);
}
static TuriValue native_seq_vec_get(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(((int64_t *)(intptr_t)v[0])[a[1].as_int]);
}
/* seq cons cell / Tuple2: both are {slot0, slot1}. */
static TuriValue native_seq_make_cell(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    int64_t *c = (int64_t *)malloc(2 * sizeof(int64_t));
    c[0] = (n >= 1) ? a[0].as_int : 0;
    c[1] = (n >= 2) ? a[1].as_int : 0;
    return turi_int((int64_t)(intptr_t)c);
}
static TuriValue native_seq_cell_fst(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[0]);
}
static TuriValue native_seq_cell_snd(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[1]);
}
static TuriValue native_seq_list_nil(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_bool(n < 1 || a[0].as_int == 0);
}
static void wk_register_seq_natives(TuriEnv *env) {
    turi_env_register_native(env, "seq-iter",            native_seq_iter,         NULL);
    turi_env_register_native(env, "seq-gen-done",        native_seq_gen_done,     NULL);
    turi_env_register_native(env, "seq-gen-next",        native_seq_gen_next,     NULL);
    turi_env_register_native(env, "seq-val-some?",       native_seq_val_some,     NULL);
    turi_env_register_native(env, "seq-val-unwrap",      native_seq_val_unwrap,   NULL);
    /* transform-layer gen<->int + driver aliases (same gen carrier). */
    turi_env_register_native(env, "seq-gen-to-int",      native_seq_gen_to_int,   NULL);
    turi_env_register_native(env, "seq-gen-from-int",    native_seq_gen_to_int,   NULL);
    turi_env_register_native(env, "seq-gen-done-int",    native_seq_gen_done,     NULL);
    turi_env_register_native(env, "seq-gen-next-int",    native_seq_gen_next,     NULL);
    /* fat-closure call bridges (one per arity/result shape). */
    turi_env_register_native(env, "seq-call-fn0",        native_seq_call_fn0,     NULL);
    turi_env_register_native(env, "seq-call-fn1",        native_seq_call_fn1,     NULL);
    turi_env_register_native(env, "seq-call-fn2",        native_seq_call_fn2,     NULL);
    turi_env_register_native(env, "seq-call-bool-fn1",   native_seq_call_bool_fn1, NULL);
    turi_env_register_native(env, "seq-call-void-fn1",   native_seq_call_void_fn1, NULL);
    /* gen-arr (gen.tur) {len, cap, data} -- also unblocks gen-collect. */
    turi_env_register_native(env, "gen-arr-new",         native_gen_arr_new,      NULL);
    turi_env_register_native(env, "gen-arr-push!",       native_gen_arr_push,     NULL);
    turi_env_register_native(env, "gen-arr-len",         native_gen_arr_len,      NULL);
    turi_env_register_native(env, "gen-arr-get",         native_gen_arr_get,      NULL);
    /* seq option box / out-vec / cons cell / Tuple2 helpers (consistent layouts:
     * all producers and accessors are nativized together). */
    turi_env_register_native(env, "seq-make-some",       native_seq_make_some,    NULL);
    turi_env_register_native(env, "seq-make-none",       native_seq_make_none,    NULL);
    turi_env_register_native(env, "seq-opt-some?",       native_seq_opt_some,     NULL);
    turi_env_register_native(env, "seq-opt-unwrap",      native_seq_opt_unwrap,   NULL);
    turi_env_register_native(env, "seq-out-vec-new",     native_seq_out_vec_new,  NULL);
    turi_env_register_native(env, "seq-out-vec-push!",   native_seq_out_vec_push, NULL);
    turi_env_register_native(env, "seq-vec-len",         native_seq_vec_len,      NULL);
    turi_env_register_native(env, "seq-vec-get",         native_seq_vec_get,      NULL);
    turi_env_register_native(env, "seq-make-cons",       native_seq_make_cell,    NULL);
    turi_env_register_native(env, "seq-make-pair",       native_seq_make_cell,    NULL);
    turi_env_register_native(env, "seq-list-head",       native_seq_cell_fst,     NULL);
    turi_env_register_native(env, "seq-list-tail",       native_seq_cell_snd,     NULL);
    turi_env_register_native(env, "seq-pair-first",      native_seq_cell_fst,     NULL);
    turi_env_register_native(env, "seq-pair-second",     native_seq_cell_snd,     NULL);
    turi_env_register_native(env, "seq-list-nil?",       native_seq_list_nil,     NULL);
}

/* JSON (stdlib/json.tur): a self-contained tagged-AST JSON engine.  The public
 * ops are inline-C wrappers over malloc'd structs + a recursive-descent parser
 * and encoder (strtoll/strtod/strncmp/memcpy), which try_exec_simple_inline_c
 * cannot run -- so the whole engine is re-implemented as natives with
 * layout-exact node structures (json-schema-interpreter-plan, Layer 1).
 *
 * Node layout: int64[2] = {type, payload}
 *   0 null   payload 0
 *   1 bool   payload 0/1
 *   2 int    payload int64 value
 *   3 float  payload = double bits (memcpy into the int64 slot)
 *   4 string payload = (char*) strdup'd
 *   5 array  payload = (tur_json_vec*) {int64* data; size_t len; size_t cap}
 *   6 object payload = head of a LIFO list of int64[3] {key, val, next}
 * Every producer/accessor below agrees bit-for-bit with the json.tur inline-C
 * so a non-nativized reader (or the encoder/decoder recursion) sees the same
 * bytes. */
typedef struct { int64_t *data; size_t len; size_t cap; } tur_json_vec;

/* Extract a NUL-terminated C string from a cstr-typed argument (it may arrive
 * tagged TURI_CSTR or as a raw pointer in the int64 carrier). */
static const char *json_arg_cstr(TuriValue v) {
    if (v.tag == TURI_CSTR) return v.as_cstr;
    return (const char *)(intptr_t)v.as_int;
}
static inline int64_t *json_node_ptr(TuriValue v) {
    return (int64_t *)(intptr_t)v.as_int;
}

/* --- builders --- */
static TuriValue native_json_null(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 0; node[1] = 0;
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_bool(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    int64_t v = 0;
    if (n >= 1) v = (a[0].tag == TURI_BOOL) ? (a[0].as_bool ? 1 : 0)
                                            : (a[0].as_int ? 1 : 0);
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 1; node[1] = v ? 1 : 0;
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 2; node[1] = (n >= 1) ? a[0].as_int : 0;
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_float(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    double v = (n >= 1) ? ((a[0].tag == TURI_FLOAT) ? a[0].as_float
                                                    : (double)a[0].as_int)
                        : 0.0;
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 3;
    memcpy(&node[1], &v, sizeof(double));
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_string(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *s = (n >= 1) ? json_arg_cstr(a[0]) : "";
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 4; node[1] = (int64_t)(intptr_t)strdup(s ? s : "");
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_array_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    tur_json_vec *v = malloc(sizeof(*v));
    v->data = NULL; v->len = 0; v->cap = 0;
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 5; node[1] = (int64_t)(intptr_t)v;
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_array_push(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *node = json_node_ptr(a[0]);
    tur_json_vec *v = (tur_json_vec *)(intptr_t)node[1];
    if (v->len >= v->cap) {
        v->cap = v->cap > 0 ? v->cap * 2 : 4;
        v->data = realloc(v->data, v->cap * sizeof(int64_t));
    }
    v->data[v->len++] = a[1].as_int;
    return a[0];
}
static TuriValue native_json_object_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 6; node[1] = 0;
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_object_put(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) return turi_int(0);
    int64_t *node = json_node_ptr(a[0]);
    const char *key = json_arg_cstr(a[1]);
    int64_t *entry = malloc(3 * sizeof(int64_t));
    entry[0] = (int64_t)(intptr_t)strdup(key ? key : "");
    entry[1] = a[2].as_int;
    entry[2] = node[1];
    node[1] = (int64_t)(intptr_t)entry;
    return a[0];
}

/* --- accessors --- */
static TuriValue native_json_type(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(json_node_ptr(a[0])[0]);
}
static TuriValue native_json_get_bool(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_bool(false);
    return turi_bool(json_node_ptr(a[0])[1] != 0);
}
static TuriValue native_json_get_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(json_node_ptr(a[0])[1]);
}
static TuriValue native_json_get_float(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_float(0.0);
    double v;
    memcpy(&v, &json_node_ptr(a[0])[1], sizeof(double));
    return turi_float(v);
}
static TuriValue native_json_get_string(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_cstr("");
    return turi_cstr((const char *)(intptr_t)json_node_ptr(a[0])[1]);
}
static TuriValue native_json_array_len(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    int64_t *node = json_node_ptr(a[0]);
    tur_json_vec *v = (tur_json_vec *)(intptr_t)node[1];
    return turi_int((int64_t)v->len);
}
static TuriValue native_json_array_get(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2 || a[0].as_int == 0) return turi_int(0);
    int64_t *node = json_node_ptr(a[0]);
    tur_json_vec *v = (tur_json_vec *)(intptr_t)node[1];
    int64_t i = a[1].as_int;
    if (i < 0 || (size_t)i >= v->len) return turi_int(0);
    return turi_int(v->data[i]);
}
/* json/get: returns a some-option box {int64 is_some; int64 value} (matching the
 * inline-C struct {bool is_some; int64 value}, padded to int64[2]) or 0 (none). */
static TuriValue native_json_get(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2 || a[0].as_int == 0) return turi_int(0);
    int64_t *node = json_node_ptr(a[0]);
    const char *key = json_arg_cstr(a[1]);
    int64_t cur = node[1];
    while (cur) {
        int64_t *ent = (int64_t *)(intptr_t)cur;
        if (strcmp((const char *)(intptr_t)ent[0], key) == 0) {
            int64_t *opt = malloc(2 * sizeof(int64_t));
            opt[0] = 1; opt[1] = ent[1];
            return turi_int((int64_t)(intptr_t)opt);
        }
        cur = ent[2];
    }
    return turi_int(0);
}
static TuriValue native_json_get_bang(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2 || a[0].as_int == 0) {
        fprintf(stderr, "json/get!: null node\n");
        abort();
    }
    int64_t *node = json_node_ptr(a[0]);
    const char *key = json_arg_cstr(a[1]);
    int64_t cur = node[1];
    while (cur) {
        int64_t *ent = (int64_t *)(intptr_t)cur;
        if (strcmp((const char *)(intptr_t)ent[0], key) == 0)
            return turi_int(ent[1]);
        cur = ent[2];
    }
    fprintf(stderr, "json/get!: key not found: %s\n", key);
    abort();
}

/* --- encoder: a growable char buffer + recursive node walk --- */
typedef struct { char *data; size_t len; size_t cap; } tur_json_encbuf;
static void json_enc_ensure(tur_json_encbuf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        b->cap = (b->len + extra + 1) * 2 + 64;
        b->data = realloc(b->data, b->cap);
    }
}
static void json_enc_append_s(tur_json_encbuf *b, const char *s) {
    size_t slen = strlen(s);
    json_enc_ensure(b, slen);
    memcpy(b->data + b->len, s, slen);
    b->len += slen;
    b->data[b->len] = '\0';
}
static void json_enc_append_c(tur_json_encbuf *b, char c) {
    json_enc_ensure(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}
static void json_enc_str(tur_json_encbuf *b, const char *s) {
    json_enc_append_c(b, '"');
    for (const char *sp = s; *sp; sp++) {
        if (*sp == '"')       json_enc_append_s(b, "\\\"");
        else if (*sp == '\\') json_enc_append_s(b, "\\\\");
        else if (*sp == '\n') json_enc_append_s(b, "\\n");
        else if (*sp == '\r') json_enc_append_s(b, "\\r");
        else if (*sp == '\t') json_enc_append_s(b, "\\t");
        else                  json_enc_append_c(b, *sp);
    }
    json_enc_append_c(b, '"');
}
static void json_enc_node(int64_t node, tur_json_encbuf *b) {
    if (!node) { json_enc_append_s(b, "null"); return; }
    int64_t *np = (int64_t *)(intptr_t)node;
    int type = (int)np[0];
    char tmp[64];
    switch (type) {
        case 0: json_enc_append_s(b, "null"); break;
        case 1: json_enc_append_s(b, np[1] ? "true" : "false"); break;
        case 2: snprintf(tmp, sizeof(tmp), "%lld", (long long)np[1]);
                json_enc_append_s(b, tmp); break;
        case 3: {
            double v; memcpy(&v, &np[1], sizeof(double));
            snprintf(tmp, sizeof(tmp), "%.17g", v);
            json_enc_append_s(b, tmp); break;
        }
        case 4: json_enc_str(b, (const char *)(intptr_t)np[1]); break;
        case 5: {
            tur_json_vec *v = (tur_json_vec *)(intptr_t)np[1];
            json_enc_append_c(b, '[');
            for (size_t i = 0; i < v->len; i++) {
                if (i > 0) json_enc_append_c(b, ',');
                json_enc_node(v->data[i], b);
            }
            json_enc_append_c(b, ']');
            break;
        }
        case 6: {
            json_enc_append_c(b, '{');
            int64_t cur = np[1];
            int first = 1;
            while (cur) {
                int64_t *ent = (int64_t *)(intptr_t)cur;
                if (!first) json_enc_append_c(b, ',');
                first = 0;
                json_enc_str(b, (const char *)(intptr_t)ent[0]);
                json_enc_append_c(b, ':');
                json_enc_node(ent[1], b);
                cur = ent[2];
            }
            json_enc_append_c(b, '}');
            break;
        }
        default: json_enc_append_s(b, "null"); break;
    }
}
static TuriValue native_json_encode(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    tur_json_encbuf b;
    b.data = malloc(256);
    b.data[0] = '\0';
    b.len = 0;
    b.cap = 256;
    json_enc_node((n >= 1) ? a[0].as_int : 0, &b);
    return turi_cstr(b.data);
}

/* --- decoder: recursive-descent over a {s, pos, err} context --- */
typedef struct { const char *s; size_t pos; int err; } tur_json_ctx;
static void json_dec_skip_ws(tur_json_ctx *c) {
    while (c->s[c->pos] == ' ' || c->s[c->pos] == '\n' ||
           c->s[c->pos] == '\r' || c->s[c->pos] == '\t') c->pos++;
}
static char *json_dec_parse_string(tur_json_ctx *c) {
    if (c->s[c->pos] != '"') { c->err = 1; return NULL; }
    c->pos++;
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    while (c->s[c->pos] && c->s[c->pos] != '"') {
        if (len + 4 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        if (c->s[c->pos] == '\\') {
            c->pos++;
            switch (c->s[c->pos]) {
                case '"':  buf[len++] = '"';  break;
                case '\\': buf[len++] = '\\'; break;
                case '/':  buf[len++] = '/';  break;
                case 'n':  buf[len++] = '\n'; break;
                case 'r':  buf[len++] = '\r'; break;
                case 't':  buf[len++] = '\t'; break;
                case 'b':  buf[len++] = '\b'; break;
                case 'f':  buf[len++] = '\f'; break;
                default:   buf[len++] = c->s[c->pos]; break;
            }
        } else {
            buf[len++] = c->s[c->pos];
        }
        c->pos++;
    }
    if (c->s[c->pos] != '"') { c->err = 1; free(buf); return NULL; }
    c->pos++;
    buf[len] = '\0';
    return buf;
}
static int64_t json_dec_parse_value(tur_json_ctx *c) {
    json_dec_skip_ws(c);
    char ch = c->s[c->pos];
    if (ch == 'n' && strncmp(c->s + c->pos, "null", 4) == 0) {
        c->pos += 4;
        int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 0; node[1] = 0;
        return (int64_t)(intptr_t)node;
    }
    if (ch == 't' && strncmp(c->s + c->pos, "true", 4) == 0) {
        c->pos += 4;
        int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 1; node[1] = 1;
        return (int64_t)(intptr_t)node;
    }
    if (ch == 'f' && strncmp(c->s + c->pos, "false", 5) == 0) {
        c->pos += 5;
        int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 1; node[1] = 0;
        return (int64_t)(intptr_t)node;
    }
    if (ch == '"') {
        char *sv = json_dec_parse_string(c);
        if (!sv) return 0;
        int64_t *node = malloc(2 * sizeof(int64_t));
        node[0] = 4; node[1] = (int64_t)(intptr_t)sv;
        return (int64_t)(intptr_t)node;
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        char *end;
        int64_t ival = strtoll(c->s + c->pos, &end, 10);
        if (*end == '.' || *end == 'e' || *end == 'E') {
            double fval = strtod(c->s + c->pos, &end);
            c->pos = (size_t)(end - c->s);
            int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 3;
            memcpy(&node[1], &fval, sizeof(double));
            return (int64_t)(intptr_t)node;
        }
        c->pos = (size_t)(end - c->s);
        int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 2; node[1] = ival;
        return (int64_t)(intptr_t)node;
    }
    if (ch == '[') {
        c->pos++;
        tur_json_vec *v = malloc(sizeof(*v));
        v->data = NULL; v->len = 0; v->cap = 0;
        json_dec_skip_ws(c);
        if (c->s[c->pos] != ']') {
            for (;;) {
                int64_t elem = json_dec_parse_value(c);
                if (c->err) { free(v->data); free(v); return 0; }
                if (v->len >= v->cap) {
                    v->cap = v->cap > 0 ? v->cap * 2 : 4;
                    v->data = realloc(v->data, v->cap * sizeof(int64_t));
                }
                v->data[v->len++] = elem;
                json_dec_skip_ws(c);
                if (c->s[c->pos] == ']') break;
                if (c->s[c->pos] != ',') { c->err = 1; free(v->data); free(v); return 0; }
                c->pos++;
            }
        }
        c->pos++;
        int64_t *node = malloc(2 * sizeof(int64_t));
        node[0] = 5; node[1] = (int64_t)(intptr_t)v;
        return (int64_t)(intptr_t)node;
    }
    if (ch == '{') {
        c->pos++;
        int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 6; node[1] = 0;
        json_dec_skip_ws(c);
        if (c->s[c->pos] != '}') {
            for (;;) {
                json_dec_skip_ws(c);
                char *key = json_dec_parse_string(c);
                if (!key || c->err) { free(node); return 0; }
                json_dec_skip_ws(c);
                if (c->s[c->pos] != ':') { c->err = 1; free(key); free(node); return 0; }
                c->pos++;
                int64_t val = json_dec_parse_value(c);
                if (c->err) { free(key); free(node); return 0; }
                int64_t *entry = malloc(3 * sizeof(int64_t));
                entry[0] = (int64_t)(intptr_t)key;
                entry[1] = val;
                entry[2] = node[1];
                node[1] = (int64_t)(intptr_t)entry;
                json_dec_skip_ws(c);
                if (c->s[c->pos] == '}') break;
                if (c->s[c->pos] != ',') { c->err = 1; free(node); return 0; }
                c->pos++;
            }
        }
        c->pos++;
        return (int64_t)(intptr_t)node;
    }
    c->err = 1;
    return 0;
}
static TuriValue native_json_decode(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    const char *s = json_arg_cstr(a[0]);
    if (!s) return turi_int(0);
    tur_json_ctx ctx; ctx.s = s; ctx.pos = 0; ctx.err = 0;
    int64_t result = json_dec_parse_value(&ctx);
    if (ctx.err) return turi_int(0);
    return turi_int(result);
}

/* --- free: no-op under the interpreter's process-lifetime policy (match the
 * signature so a call type-checks and is a harmless no-op). --- */
static TuriValue native_json_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_nil();
}

static void wk_register_json_natives(TuriEnv *env) {
    /* builders */
    turi_env_register_native(env, "json/null",       native_json_null,       NULL);
    turi_env_register_native(env, "json/bool",       native_json_bool,       NULL);
    turi_env_register_native(env, "json/int",        native_json_int,        NULL);
    turi_env_register_native(env, "json/float",      native_json_float,      NULL);
    turi_env_register_native(env, "json/string",     native_json_string,     NULL);
    turi_env_register_native(env, "json/array-new",  native_json_array_new,  NULL);
    turi_env_register_native(env, "json/array-push", native_json_array_push, NULL);
    turi_env_register_native(env, "json/object-new", native_json_object_new, NULL);
    turi_env_register_native(env, "json/object-put", native_json_object_put, NULL);
    /* accessors */
    turi_env_register_native(env, "json/type",       native_json_type,       NULL);
    turi_env_register_native(env, "json/get-bool",   native_json_get_bool,   NULL);
    turi_env_register_native(env, "json/get-int",    native_json_get_int,    NULL);
    turi_env_register_native(env, "json/get-float",  native_json_get_float,  NULL);
    turi_env_register_native(env, "json/get-string", native_json_get_string, NULL);
    turi_env_register_native(env, "json/array-len",  native_json_array_len,  NULL);
    turi_env_register_native(env, "json/array-get",  native_json_array_get,  NULL);
    turi_env_register_native(env, "json/get",        native_json_get,        NULL);
    turi_env_register_native(env, "json/get!",       native_json_get_bang,   NULL);
    /* encoder / decoder / free */
    turi_env_register_native(env, "json/encode",     native_json_encode,     NULL);
    turi_env_register_native(env, "json/decode",     native_json_decode,     NULL);
    turi_env_register_native(env, "json/free",       native_json_free,       NULL);
}

/* SCHEMA (stdlib/schema.tur): the runtime schema validator built on top of the
 * tagged JSON nodes (Layer 2 of turi-json-schema-interpreter-plan).  A schema
 * node is int64[4]{kind,a,b,c} with the SCHEMA_* discriminants; the decoder
 * (sch-decode-rec-) recursively walks (schema, json-node, path) accumulating
 * SchemaError records into a {data,len,cap} vector.  The constructors malloc
 * tagged structs and the decoder recurses + invokes transform/fmap/ap fat
 * closures via a C function pointer -- neither runnable by the simple inline-C
 * executor -- so the whole engine is re-implemented natively with layout-exact
 * structs, and the fat-closure call points route through turi_call (the closure
 * is a TURI_CLOSURE under --interpret, same shape as the seq bridges).
 *
 * Schema node:   int64[4]{kind, a, b, c}
 * Object fields: s[1]=count s[2]=cap s[3]=(int64* data) of {key,inner} pairs
 * SchemaError:   int64[3]{char* path, char* msg, int64 val}
 * Error vec:     {int64* data; size_t len; size_t cap}  (== int64[3])
 * Result:        {bool is_ok; int64 ok_val; int64 err_val} (== int64[3]:
 *                [0]=is_ok [1]=ok_val [2]=err_val). */
typedef struct { int64_t kind, a, b, c; } tur_sch_t;

static const char *sch_tyname(int64_t jtype) {
    switch (jtype) {
        case 0: return ":nil";    case 1: return ":bool";
        case 2: return ":int";    case 3: return ":float";
        case 4: return ":cstr";   case 5: return ":array";
        case 6: return ":object"; default: return ":unknown";
    }
}
static const char *sch_want_name(int64_t kind) {
    switch (kind) {
        case 0: return ":cstr";  case 1: return ":int";
        case 2: return ":float"; case 3: return ":bool";
        case 4: return ":nil";   case 6: return ":object";
        case 7: return ":array"; default: return "value";
    }
}
static int64_t sch_jtype(int64_t node) {
    if (!node) return -1;
    return (int64_t)(int)((int64_t *)(intptr_t)node)[0];
}
static int64_t sch_jpayload(int64_t node) {
    return ((int64_t *)(intptr_t)node)[1];
}
static void sch_vpush(tur_json_vec *v, int64_t x) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 4;
        v->data = realloc(v->data, v->cap * sizeof(int64_t));
    }
    v->data[v->len++] = x;
}
static void sch_push_err(tur_json_vec *errs, const char *path,
                         const char *msg, int64_t val) {
    int64_t *e = malloc(3 * sizeof(int64_t));
    e[0] = (int64_t)(intptr_t)strdup(path ? path : "");
    e[1] = (int64_t)(intptr_t)strdup(msg);
    e[2] = val;
    sch_vpush(errs, (int64_t)(intptr_t)e);
}
static char *sch_mkpath(const char *base, const char *key) {
    if (!base || !base[0]) return strdup(key);
    size_t n = strlen(base) + strlen(key) + 2;
    char *out = malloc(n);
    snprintf(out, n, "%s.%s", base, key);
    return out;
}
static char *sch_mkidx(const char *base, int64_t idx) {
    const char *b = base ? base : "";
    size_t n = strlen(b) + 24;
    char *out = malloc(n);
    snprintf(out, n, "%s[%lld]", b, (long long)idx);
    return out;
}
/* Invoke a stored fat closure (carried as the int64 pointer in the schema node)
 * with one int64-carrier argument, returning the result as an int64 carrier. */
static int64_t sch_apply1(TuriEnv *env, int64_t fn_carrier, int64_t arg) {
    TuriValue av = turi_int(arg);
    TuriValue r = turi_call(env, seq_as_closure(turi_int(fn_carrier)), &av, 1);
    if (turi_is_error(r) || env->throwing) {
        /* returns a raw int64 carrier; promote a value-level error to a throw so
         * the enclosing schema native and the driver propagate it. */
        if (turi_is_error(r)) { env->throwing = true; env->throw_value = r; }
        return 0;
    }
    if (r.tag == TURI_FLOAT) { int64_t b; memcpy(&b, &r.as_float, 8); return b; }
    if (r.tag == TURI_BOOL)  return r.as_bool ? 1 : 0;
    return r.as_int;
}

/* The recursive decoder -- mirrors sch-decode-rec- (schema.tur) bit-for-bit,
 * with TUR_APPLY1 / the kind-14 C call routed through turi_call. */
static int64_t sch_decode_rec(TuriEnv *env, int64_t schema, int64_t node,
                              const char *path, tur_json_vec *ep) {
    tur_sch_t *s = (tur_sch_t *)(intptr_t)schema;
    if (!s) return 0;
    char buf[128];
    int64_t jt = sch_jtype(node);
    int64_t jp = node ? sch_jpayload(node) : 0;
    switch (s->kind) {
        case 0: case 1: case 2: case 3: case 4: { /* scalars */
            int64_t want = (s->kind == 0) ? 4 : (s->kind == 1) ? 2 :
                           (s->kind == 2) ? 3 : (s->kind == 3) ? 1 : 0;
            if (jt != want) {
                snprintf(buf, sizeof(buf), "expected %s, got %s",
                         sch_want_name(s->kind), sch_tyname(jt));
                sch_push_err(ep, path, buf, node);
                return 0;
            }
            return jp;
        }
        case 5: { /* literal */
            if (s->a == 2) {
                if (jt != 2 || jp != s->b) {
                    snprintf(buf, sizeof(buf), "expected literal %lld", (long long)s->b);
                    sch_push_err(ep, path, buf, node);
                    return 0;
                }
                return s->b;
            } else {
                const char *want = (const char *)(intptr_t)s->b;
                if (jt != 4 || strcmp((const char *)(intptr_t)jp, want) != 0) {
                    snprintf(buf, sizeof(buf), "expected literal \"%s\"", want);
                    sch_push_err(ep, path, buf, node);
                    return 0;
                }
                return jp;
            }
        }
        case 6: { /* object */
            if (jt != 6) {
                snprintf(buf, sizeof(buf), "expected :object, got %s", sch_tyname(jt));
                sch_push_err(ep, path, buf, node);
                return 0;
            }
            int64_t fcount = s->a;
            int64_t *fdata = (int64_t *)(intptr_t)s->c;
            if (fdata) {
                for (int64_t i = 0; i < fcount; i++) {
                    const char *key = (const char *)(intptr_t)fdata[i * 2];
                    tur_sch_t *fsch = (tur_sch_t *)(intptr_t)fdata[i * 2 + 1];
                    int64_t *on = (int64_t *)(intptr_t)node;
                    int64_t cur = on[1]; int64_t found = 0; int present = 0;
                    while (cur) {
                        int64_t *ent = (int64_t *)(intptr_t)cur;
                        if (strcmp((const char *)(intptr_t)ent[0], key) == 0) {
                            found = ent[1]; present = 1; break;
                        }
                        cur = ent[2];
                    }
                    char *fpath = sch_mkpath(path, key);
                    if (!present) {
                        if (fsch && fsch->kind == 8) { free(fpath); continue; }
                        sch_push_err(ep, fpath, "missing required field", 0);
                    } else {
                        sch_decode_rec(env, (int64_t)(intptr_t)fsch, found, fpath, ep);
                    }
                    free(fpath);
                }
            }
            return node;
        }
        case 7: { /* array */
            if (jt != 5) {
                snprintf(buf, sizeof(buf), "expected :array, got %s", sch_tyname(jt));
                sch_push_err(ep, path, buf, node);
                return 0;
            }
            int64_t elem = s->a;
            tur_json_vec *jarr = (tur_json_vec *)(intptr_t)jp;
            tur_json_vec *out = malloc(sizeof(*out));
            out->data = NULL; out->len = 0; out->cap = 0;
            if (jarr) {
                for (size_t i = 0; i < jarr->len; i++) {
                    char *ipath = sch_mkidx(path, (int64_t)i);
                    int64_t dv = sch_decode_rec(env, elem, jarr->data[i], ipath, ep);
                    free(ipath);
                    sch_vpush(out, dv);
                }
            }
            return (int64_t)(intptr_t)out;
        }
        case 8: { /* optional */
            if (jt == 0 || node == 0) return 0;
            return sch_decode_rec(env, s->a, node, path, ep);
        }
        case 9: { /* union: first arm that matches wins */
            tur_json_vec *arms = (tur_json_vec *)(intptr_t)s->a;
            if (!arms || arms->len == 0) return 0;
            for (size_t i = 0; i < arms->len; i++) {
                size_t before = ep->len;
                int64_t dv = sch_decode_rec(env, arms->data[i], node, path, ep);
                if (ep->len == before) return dv;
                ep->len = before;
            }
            snprintf(buf, sizeof(buf), "no union arm matched (got %s)", sch_tyname(jt));
            sch_push_err(ep, path, buf, node);
            return 0;
        }
        case 10: { /* transform / fmap */
            size_t before = ep->len;
            int64_t dv = sch_decode_rec(env, s->a, node, path, ep);
            if (ep->len != before) return 0;
            return sch_apply1(env, s->b, dv);
        }
        case 11: { /* recursive */
            if (s->c == 0) {
                s->c = sch_apply1(env, s->a, (int64_t)(intptr_t)s);
            }
            return sch_decode_rec(env, s->c, node, path, ep);
        }
        case 12: return s->a; /* always */
        case 13: /* never */
            sch_push_err(ep, path, (const char *)(intptr_t)s->a, node);
            return 0;
        case 14: case 16: { /* ap / ap-fat: apply decoded fn arm to arg arm */
            size_t before = ep->len;
            int64_t fv = sch_decode_rec(env, s->a, node, path, ep);
            int64_t av = sch_decode_rec(env, s->b, node, path, ep);
            if (ep->len != before) return 0;
            return sch_apply1(env, fv, av);
        }
        case 15: { /* field-of */
            if (jt != 6) {
                snprintf(buf, sizeof(buf), "expected :object, got %s", sch_tyname(jt));
                sch_push_err(ep, path, buf, node);
                return 0;
            }
            const char *key = (const char *)(intptr_t)s->a;
            tur_sch_t *inner = (tur_sch_t *)(intptr_t)s->b;
            int64_t *on = (int64_t *)(intptr_t)node;
            int64_t cur = on[1]; int64_t found = 0; int present = 0;
            while (cur) {
                int64_t *ent = (int64_t *)(intptr_t)cur;
                if (strcmp((const char *)(intptr_t)ent[0], key) == 0) {
                    found = ent[1]; present = 1; break;
                }
                cur = ent[2];
            }
            char *fpath = sch_mkpath(path, key);
            int64_t result;
            if (!present) {
                if (inner && inner->kind == 8) { result = 0; }
                else { sch_push_err(ep, fpath, "missing required field", 0); result = 0; }
            } else {
                result = sch_decode_rec(env, (int64_t)(intptr_t)inner, found, fpath, ep);
            }
            free(fpath);
            return result;
        }
        default: return 0;
    }
}

/* --- schema node constructors (each mallocs int64[4]{kind,a,b,c}) --- */
static int64_t *sch_alloc(int64_t kind, int64_t a, int64_t b, int64_t c) {
    int64_t *s = malloc(4 * sizeof(int64_t));
    s[0] = kind; s[1] = a; s[2] = b; s[3] = c;
    return s;
}
#define SCH_RET(p) turi_int((int64_t)(intptr_t)(p))
static TuriValue native_schema_str(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(0, 0, 0, 0));
}
static TuriValue native_schema_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(1, 0, 0, 0));
}
static TuriValue native_schema_float(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(2, 0, 0, 0));
}
static TuriValue native_schema_bool(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(3, 0, 0, 0));
}
static TuriValue native_schema_nil(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(4, 0, 0, 0));
}
static TuriValue native_schema_literal_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(5, 2, (n >= 1) ? a[0].as_int : 0, 0));
}
static TuriValue native_schema_literal_str(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *v = (n >= 1) ? json_arg_cstr(a[0]) : "";
    return SCH_RET(sch_alloc(5, 4, (int64_t)(intptr_t)strdup(v ? v : ""), 0));
}
static TuriValue native_schema_object_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(6, 0, 0, 0));
}
static TuriValue native_schema_field(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) return turi_int(0);
    int64_t *s = json_node_ptr(a[0]);
    const char *key = json_arg_cstr(a[1]);
    int64_t count = s[1], cap = s[2];
    int64_t *data = (int64_t *)(intptr_t)s[3];
    if (count == cap) {
        cap = cap ? cap * 2 : 4;
        data = realloc(data, cap * 2 * sizeof(int64_t));
        s[2] = cap;
        s[3] = (int64_t)(intptr_t)data;
    }
    data[count * 2]     = (int64_t)(intptr_t)strdup(key ? key : "");
    data[count * 2 + 1] = a[2].as_int;
    s[1] = count + 1;
    return a[0];
}
static TuriValue native_schema_array(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(7, (n >= 1) ? a[0].as_int : 0, 0, 0));
}
static TuriValue native_schema_optional(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(8, (n >= 1) ? a[0].as_int : 0, 0, 0));
}
static TuriValue native_schema_union(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(9, (n >= 1) ? a[0].as_int : 0, 0, 0));
}
static TuriValue native_schema_transform(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    return SCH_RET(sch_alloc(10, a[0].as_int, a[1].as_int, 0));
}
static TuriValue native_schema_rec(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(11, (n >= 1) ? a[0].as_int : 0, 0, 0));
}
static TuriValue native_schema_kind(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(-1);
    return turi_int(json_node_ptr(a[0])[0]);
}
static TuriValue native_schema_always(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(12, (n >= 1) ? a[0].as_int : 0, 0, 0));
}
static TuriValue native_schema_never(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *m = (n >= 1) ? json_arg_cstr(a[0]) : "";
    return SCH_RET(sch_alloc(13, (int64_t)(intptr_t)strdup(m ? m : ""), 0, 0));
}
static TuriValue native_schema_ap(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    return SCH_RET(sch_alloc(14, a[0].as_int, a[1].as_int, 0));
}
static TuriValue native_schema_ap_fat(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    return SCH_RET(sch_alloc(16, a[0].as_int, a[1].as_int, 0));
}
static TuriValue native_schema_field_of(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    const char *key = json_arg_cstr(a[0]);
    return SCH_RET(sch_alloc(15, (int64_t)(intptr_t)strdup(key ? key : ""),
                             a[1].as_int, 0));
}
#undef SCH_RET

/* --- SchemaError accessors --- */
static TuriValue native_schema_error_path(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_cstr("");
    return turi_cstr((const char *)(intptr_t)json_node_ptr(a[0])[0]);
}
static TuriValue native_schema_error_text(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_cstr("");
    return turi_cstr((const char *)(intptr_t)json_node_ptr(a[0])[1]);
}
static TuriValue native_schema_error_count(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    tur_json_vec *v = (tur_json_vec *)(intptr_t)a[0].as_int;
    return turi_int((int64_t)v->len);
}
static TuriValue native_schema_error_at(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2 || a[0].as_int == 0) return turi_int(0);
    tur_json_vec *v = (tur_json_vec *)(intptr_t)a[0].as_int;
    int64_t i = a[1].as_int;
    if (i < 0 || (size_t)i >= v->len) return turi_int(0);
    return turi_int(v->data[i]);
}
static TuriValue native_schema_error_message(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    tur_json_vec *v = (n >= 1) ? (tur_json_vec *)(intptr_t)a[0].as_int : NULL;
    if (!v || v->len == 0) { char *empty = malloc(1); empty[0] = 0; return turi_cstr(empty); }
    size_t total = 1;
    for (size_t i = 0; i < v->len; i++) {
        int64_t *er = (int64_t *)(intptr_t)v->data[i];
        const char *path = (const char *)(intptr_t)er[0];
        const char *msg  = (const char *)(intptr_t)er[1];
        total += strlen(msg) + 2;
        if (path && path[0]) total += strlen(path) + 2;
    }
    char *out = malloc(total);
    out[0] = 0;
    for (size_t i = 0; i < v->len; i++) {
        int64_t *er = (int64_t *)(intptr_t)v->data[i];
        const char *path = (const char *)(intptr_t)er[0];
        const char *msg  = (const char *)(intptr_t)er[1];
        if (i > 0) strcat(out, "\n");
        if (path && path[0]) { strcat(out, path); strcat(out, ": "); }
        strcat(out, msg);
    }
    return turi_cstr(out);
}

/* --- decoder entry points + Result accessors --- */
static TuriValue native_schema_decode(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    tur_json_vec *errs = malloc(sizeof(*errs));
    errs->data = NULL; errs->len = 0; errs->cap = 0;
    int64_t value = sch_decode_rec(e, a[0].as_int, a[1].as_int, "", errs);
    int64_t *r = malloc(3 * sizeof(int64_t));
    if (errs->len == 0) {
        r[0] = 1; r[1] = value; r[2] = 0;
        free(errs->data); free(errs);
    } else {
        r[0] = 0; r[1] = 0; r[2] = (int64_t)(intptr_t)errs;
    }
    return turi_int((int64_t)(intptr_t)r);
}
static TuriValue native_schema_decode_ok(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_bool(false);
    return turi_bool(json_node_ptr(a[0])[0] != 0);
}
static TuriValue native_schema_decode_value(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(json_node_ptr(a[0])[1]);
}
static TuriValue native_schema_decode_errors(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(json_node_ptr(a[0])[2]);
}
static TuriValue native_schema_decode_abort(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    tur_json_vec *v = (n >= 1) ? (tur_json_vec *)(intptr_t)a[0].as_int : NULL;
    fprintf(stderr, "schema-decode!: validation failed\n");
    if (v) for (size_t i = 0; i < v->len; i++) {
        int64_t *er = (int64_t *)(intptr_t)v->data[i];
        const char *path = (const char *)(intptr_t)er[0];
        const char *msg  = (const char *)(intptr_t)er[1];
        if (path && path[0]) fprintf(stderr, "  %s: %s\n", path, msg);
        else                 fprintf(stderr, "  %s\n", msg);
    }
    /* A catchable panic, not abort(): this is the interpreter half of the
     * change in stdlib/schema.tur that lets `#json-str?<T>` (a catch-unwind
     * around the panicking decode) recover from a schema violation.  With no
     * catch boundary in scope turi_runtime_panic still prints and exits, so a
     * plain `schema-decode!` failure dies as loudly as it always did. */
    turi_runtime_panic(e, "schema-decode!: validation failed");
    return turi_int(0);
}

static void wk_register_schema_natives(TuriEnv *env) {
    /* constructors */
    turi_env_register_native(env, "schema/str",         native_schema_str,         NULL);
    turi_env_register_native(env, "schema/int",         native_schema_int,         NULL);
    turi_env_register_native(env, "schema/float",       native_schema_float,       NULL);
    turi_env_register_native(env, "schema/bool",        native_schema_bool,        NULL);
    turi_env_register_native(env, "schema/nil",         native_schema_nil,         NULL);
    turi_env_register_native(env, "schema/literal-int", native_schema_literal_int, NULL);
    turi_env_register_native(env, "schema/literal-str", native_schema_literal_str, NULL);
    turi_env_register_native(env, "schema/object-new",  native_schema_object_new,  NULL);
    turi_env_register_native(env, "schema/field",       native_schema_field,       NULL);
    turi_env_register_native(env, "schema/array",       native_schema_array,       NULL);
    turi_env_register_native(env, "schema/optional",    native_schema_optional,    NULL);
    turi_env_register_native(env, "schema/union",       native_schema_union,       NULL);
    turi_env_register_native(env, "schema/transform",   native_schema_transform,   NULL);
    turi_env_register_native(env, "schema/rec",         native_schema_rec,         NULL);
    turi_env_register_native(env, "schema/kind",        native_schema_kind,        NULL);
    turi_env_register_native(env, "schema/always",      native_schema_always,      NULL);
    turi_env_register_native(env, "schema/never",       native_schema_never,       NULL);
    turi_env_register_native(env, "schema/ap",          native_schema_ap,          NULL);
    turi_env_register_native(env, "schema/ap-fat",      native_schema_ap_fat,      NULL);
    turi_env_register_native(env, "schema/field-of",    native_schema_field_of,    NULL);
    turi_env_register_native(env, "schema/fmap",        native_schema_transform,   NULL);
    /* error accessors */
    turi_env_register_native(env, "schema-error-path",    native_schema_error_path,    NULL);
    turi_env_register_native(env, "schema-error-text",    native_schema_error_text,    NULL);
    turi_env_register_native(env, "schema-error-count",   native_schema_error_count,   NULL);
    turi_env_register_native(env, "schema-error-at",      native_schema_error_at,      NULL);
    turi_env_register_native(env, "schema-error-message", native_schema_error_message, NULL);
    /* decoder + result accessors */
    turi_env_register_native(env, "schema-decode",        native_schema_decode,        NULL);
    turi_env_register_native(env, "schema-decode-ok?",    native_schema_decode_ok,     NULL);
    turi_env_register_native(env, "schema-decode-value",  native_schema_decode_value,  NULL);
    turi_env_register_native(env, "schema-decode-errors", native_schema_decode_errors, NULL);
    turi_env_register_native(env, "schema-decode-abort",  native_schema_decode_abort,  NULL);
}

/* SYM (turi): first-class :Sym natives (-Xsymbols).  The interpreter carries a
 * :Sym as a stable `const Symbol *` (interned in env->st by the EX_SYM_LIT case
 * in eval.c) in the int64 carrier.  These override the inline-C sym.tur bodies
 * (sym->str / sym=? / Eq[Sym] / Hash[Sym]), which deref a `struct __tur_sym`
 * the tree-walker cannot evaluate, and provide str->sym for sym-dynamic.tur. */
static TuriValue native_sym_to_str(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_nil();
    const Symbol *s = (const Symbol *)(intptr_t)a[0].as_int;
    if (!s) return turi_nil();
    return turi_cstr(s->name);
}
static TuriValue native_sym_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_bool(false);
    /* Symbols are interned: pointer identity is content equality. */
    return turi_bool(a[0].as_int == a[1].as_int);
}
static TuriValue native_sym_hash(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    const Symbol *s = (const Symbol *)(intptr_t)a[0].as_int;
    return turi_int(s ? (int64_t)s->hash : 0);
}
static TuriValue native_str_to_sym(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1) return turi_nil();
    const char *str = (a[0].tag == TURI_CSTR)
                        ? a[0].as_cstr
                        : (const char *)(intptr_t)a[0].as_int;
    if (!str) return turi_nil();
    StrSlice sl = { str, (uint32_t)strlen(str) };
    const Symbol *s = symtab_intern(&e->st, sl);
    TuriValue v = {0};
    v.tag = TURI_INT;
    v.as_int = (int64_t)(intptr_t)s;
    return v;
}
static void wk_register_sym_natives(TuriEnv *env) {
    turi_env_register_native(env, "sym->str",  native_sym_to_str, NULL);
    turi_env_register_native(env, "sym=?",      native_sym_eq,     NULL);
    turi_env_register_native(env, "str->sym",   native_str_to_sym, NULL);
    /* Typeclass instance methods: Eq[Sym].eq?, Hash[Sym].hash (pointer identity
     * / precomputed hash).  MapKey[Sym] reuses the int-carrier comparator already
     * registered for scalar keys, so no Sym-specific mk-* native is needed. */
    turi_env_register_native(env, "__inst_Eq_eq_qu_Sym", native_sym_eq,   NULL);
    turi_env_register_native(env, "__inst_Hash_hash_Sym", native_sym_hash, NULL);
    /* MapKey[Sym].mk-cmp returns the carrier comparator address; symbols compare
     * by pointer identity, so reuse the int-carrier eq comparator (its inline-C
     * body returns a captured C function-pointer address the tree-walker cannot
     * evaluate).  mk-box (identity) and mk-owned? (0) are plain bodies the simple
     * inline-C executor already handles. */
    turi_env_register_native(env, "__inst_MapKey_mk_hycmp_Sym", native_mk_cmp_int, NULL);
    /* MapKey[Sym].mk-box is inline-C (`(int64_t)(intptr_t)x`) like the cstr/float
     * boxes (the int box is pure-turi `x` and needs none); reuse the cstr native,
     * which returns the carrier word unchanged -- the Sym pointer. */
    turi_env_register_native(env, "__inst_MapKey_mk_hybox_Sym", native_mk_box_cstr, NULL);
}


/* -------------------------------------------------------------------------
 * Slice operations
 * Slice represented as int64_t[2]: [0]=data ptr, [1]=len
 * ---------------------------------------------------------------------- */
static TuriValue native_slice_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t *s = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!s) return turi_nil();
    s[0] = (n >= 1) ? a[0].as_int : 0;
    s[1] = (n >= 2) ? a[1].as_int : 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)s; return v;
}
static TuriValue native_slice_len(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *s = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(s ? s[1] : 0);
}
static TuriValue native_slice_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *s = (int64_t *)(intptr_t)a[0].as_int;
    int64_t  i = a[1].as_int;
    if (!s || i < 0 || i >= s[1]) {
        fprintf(stderr, "slice index out of bounds\n"); fflush(stderr); _exit(1);
    }
    int64_t *data = (int64_t *)(intptr_t)s[0];
    return turi_int(data[i]);
}
static TuriValue native_slice_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n >= 1) { void *p = (void *)(intptr_t)a[0].as_int; if (p) free(p); }
    return turi_nil();
}

/* Typed-list (stdlib/list.tur) carrier-level length.  A Cons cell is a malloc'd
 * { int64_t head; int64_t tail; }; its pointer is the int64 carrier and tnil is
 * 0 -- exactly the layout list.tur's inline-C uses, so this reproduces
 * list-length (which the tree-walker cannot run as inline-C) by walking the
 * chain.  The carrier ctor + head/tail accessors already exist as native_cons /
 * native_list_head / native_list_tail (registered for the untyped head/tail/cons
 * benchmark surface); they read the same box, so list.tur's typed tcons /
 * list-head / list-tail bind to them too.  The first cell may also be a
 * make-struct Cons TuriStruct (thead/ttail's representation) -- handled here
 * defensively, then the carrier tail is followed. */
static TuriValue native_list_length(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t count = 0;
    TuriValue node = a[0];
    for (;;) {
        if (node.tag == TURI_STRUCT) {
            /* interpreter representation: tcons builds a make-struct Cons whose
             * tail field (index 1) is the next cell -- itself a Cons struct or
             * the int 0 nil sentinel.  Walk the struct chain. */
            count++;
            bool f = false; node = turi_struct_field(node, 1, &f);
            if (!f) break;
            continue;
        }
        /* carrier representation: a malloc'd { head, tail } box chain (the
         * untyped cons/head/tail benchmark surface); 0 is nil. */
        int64_t ptr = node.as_int;
        while (ptr) {
            count++;
            int64_t *cell = (int64_t *)(intptr_t)ptr;
            ptr = cell[1];
        }
        break;
    }
    return turi_int(count);
}

/* Free monad (stdlib/free.tur) natives.  free.tur's free-bind / free-fmap /
 * free-run have #{Unsafe} inline-C bodies that cast the Free ADT carrier to a C
 * tagged-union struct and invoke the ^fat continuation through a tur_poly_fn_t.
 * Under --interpret the Free value is a TuriStruct (the ADT constructor
 * PureFree/Suspend built by adt_ctor_native), and the continuation is a turi
 * closure -- so these natives read the tag by struct name, read the payload
 * (field 0 for both arms), and call the continuation via turi_call. */
static bool free_is_ctor(TuriValue v, const char *name) {
    const char *nm = turi_struct_name(v);
    return nm && strcmp(nm, name) == 0;
}
/* Invoke a ^fat continuation that may arrive as a closure or as an int64
 * carrier holding the TuriClosure* (the carrier-readback case). */
static TuriValue free_call_fat(TuriEnv *env, TuriValue k, TuriValue arg) {
    if (k.tag == TURI_INT && k.as_int != 0) {
        TuriClosure *cl = (TuriClosure *)(intptr_t)k.as_int;
        k.tag = TURI_CLOSURE; k.as_closure = cl;
    }
    return turi_call(env, k, &arg, 1);
}
/* free-bind : (ma  ^fat kont) -- PureFree x => kont(x); Suspend => pass through. */
static TuriValue native_free_bind(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    if (free_is_ctor(a[0], "PureFree")) {
        bool f = false; TuriValue x = turi_struct_field(a[0], 0, &f);
        return free_call_fat(env, a[1], x);
    }
    return a[0]; /* Suspend -- pass through */
}
/* free-run : (^fat interp  free) -- PureFree x => x; Suspend fx => interp(fx). */
static TuriValue native_free_run(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    if (free_is_ctor(a[1], "PureFree")) {
        bool f = false; TuriValue x = turi_struct_field(a[1], 0, &f);
        return x;
    }
    bool f = false; TuriValue inner = turi_struct_field(a[1], 0, &f); /* Suspend fx */
    return free_call_fat(env, a[0], inner);
}

/* str->int-checked (str.tur): inline-C strtoll that returns an Either -- a
 * (Left code) on failure, (Right n) on a clean parse.  Re-implemented as a
 * native that builds the Either ADT via turi_make_struct (a cstr arrives as an
 * int64 carrier holding the char*, same as native_cstr_parse_int reads). */
static TuriValue native_str_to_int_checked(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *cs = (n >= 1) ? (const char *)(intptr_t)a[0].as_int : NULL;
    if (!cs || *cs == '\0') {
        TuriValue f[1] = { turi_int(0) };           /* empty -> Left 0 */
        return turi_make_struct(env, "Left", f, 1);
    }
    char *end = NULL;
    long long v = strtoll(cs, &end, 10);
    if (end == cs || *end != '\0') {
        TuriValue f[1] = { turi_int(1) };           /* garbage -> Left 1 */
        return turi_make_struct(env, "Left", f, 1);
    }
    TuriValue f[1] = { turi_int((int64_t)v) };
    return turi_make_struct(env, "Right", f, 1);
}

/* Grid (stdlib/grid.tur) natives.  grid.tur's ops are inline-C over a
 * { int64_t *data; int width; int height; int cx; int cy; } header (the grid
 * handle is the int64 carrier of that pointer; cells are row-major int64).
 * Re-implemented over the identical layout so the tree-walker can run them. */
typedef struct { int64_t *data; int width; int height; int cx; int cy; } TuriGridRep;
static TuriValue native_grid_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t w = (n >= 1) ? a[0].as_int : 0;
    int64_t h = (n >= 2) ? a[1].as_int : 0;
    TuriGridRep *g = (TuriGridRep *)malloc(sizeof(*g));
    if (!g) return turi_int(0);
    g->width = (int)w; g->height = (int)h; g->cx = 0; g->cy = 0;
    int64_t cells = w * h; if (cells < 0) cells = 0;
    g->data = (int64_t *)calloc((size_t)cells, sizeof(int64_t));
    return turi_int((int64_t)(intptr_t)g);
}
static TuriValue native_grid_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_int(0);
    TuriGridRep *g = (TuriGridRep *)(intptr_t)a[0].as_int;
    if (!g || !g->data) return turi_int(0);
    int64_t x = a[1].as_int, y = a[2].as_int;
    return turi_int(g->data[(size_t)(y * g->width + x)]);
}
static TuriValue native_grid_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 4) return turi_nil();
    TuriGridRep *g = (TuriGridRep *)(intptr_t)a[0].as_int;
    if (!g || !g->data) return turi_nil();
    int64_t x = a[1].as_int, y = a[2].as_int, v = a[3].as_int;
    g->data[(size_t)(y * g->width + x)] = v;
    return turi_nil();
}
static TuriValue native_grid_width(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriGridRep *g = (TuriGridRep *)(intptr_t)a[0].as_int;
    return turi_int(g ? g->width : 0);
}
static TuriValue native_grid_height(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriGridRep *g = (TuriGridRep *)(intptr_t)a[0].as_int;
    return turi_int(g ? g->height : 0);
}
static TuriValue native_grid_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    TuriGridRep *g = (TuriGridRep *)(intptr_t)a[0].as_int;
    if (g) { if (g->data) free(g->data); free(g); }
    return turi_nil();
}

/* SizedBuf (stdlib/sized-buf.tur) natives.  The user-facing sized-buf-* ops are
 * thin pure-turi wrappers over these __sized-buf-*-raw #{Unsafe} inline-C
 * primitives, which operate on a { int64_t len; int64_t *data; } header (the
 * SizedBuf carrier is the int64 of that pointer).  Re-implemented over the
 * identical layout. */
typedef struct { int64_t len; int64_t *data; } TuriSizedBufRep;
static TuriSizedBufRep *sbuf_of(TuriValue v) { return (TuriSizedBufRep *)(intptr_t)v.as_int; }
static TuriValue native_sbuf_new_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t k = (n >= 1) ? a[0].as_int : 0;
    TuriSizedBufRep *b = (TuriSizedBufRep *)malloc(sizeof(*b));
    if (!b) return turi_int(0);
    b->len = k;
    b->data = k > 0 ? (int64_t *)malloc((size_t)k * sizeof(int64_t)) : NULL;
    return turi_int((int64_t)(intptr_t)b);
}
static TuriValue native_sbuf_new_zeroed_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t k = (n >= 1) ? a[0].as_int : 0;
    TuriSizedBufRep *b = (TuriSizedBufRep *)malloc(sizeof(*b));
    if (!b) return turi_int(0);
    b->len = k;
    b->data = k > 0 ? (int64_t *)calloc((size_t)k, sizeof(int64_t)) : NULL;
    return turi_int((int64_t)(intptr_t)b);
}
static TuriValue native_sbuf_free_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    TuriSizedBufRep *b = sbuf_of(a[0]);
    if (b) { if (b->data) free(b->data); free(b); }
    return turi_nil();
}
static TuriValue native_sbuf_len_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]);
    return turi_int(b ? b->len : 0);
}
static TuriValue native_sbuf_get_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]); int64_t i = a[1].as_int;
    if (b && i >= 0 && i < b->len) return turi_int(b->data[i]);
    fprintf(stderr, "sized-buf-get: index out of bounds\n"); _exit(1);
    return turi_int(0);
}
static TuriValue native_sbuf_set_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]); int64_t i = a[1].as_int, v = a[2].as_int;
    if (b && i >= 0 && i < b->len) { b->data[i] = v; return a[0]; }
    fprintf(stderr, "sized-buf-set!: index out of bounds\n"); _exit(1);
    return a[0];
}
static TuriValue native_sbuf_fill_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]); int64_t v = a[1].as_int;
    if (b) for (int64_t i = 0; i < b->len; i++) b->data[i] = v;
    return a[0];
}
static TuriValue native_sbuf_copy_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TuriSizedBufRep *d = sbuf_of(a[0]), *s = sbuf_of(a[1]);
    if (!d || !s) return a[0];
    if (d->len != s->len) {
        fprintf(stderr, "sized-buf-copy!: length mismatch (%lld vs %lld)\n",
                (long long)d->len, (long long)s->len); _exit(1);
    }
    if (d->len > 0) memcpy(d->data, s->data, (size_t)d->len * sizeof(int64_t));
    return a[0];
}
static TuriValue native_sbuf_sum_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]); int64_t s = 0;
    if (b) for (int64_t i = 0; i < b->len; i++) s += b->data[i];
    return turi_int(s);
}
static TuriValue native_sbuf_min_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]);
    if (!b || b->len == 0) { fprintf(stderr, "sized-buf-min: empty buffer\n"); _exit(1); return turi_int(0); }
    int64_t m = b->data[0];
    for (int64_t i = 1; i < b->len; i++) if (b->data[i] < m) m = b->data[i];
    return turi_int(m);
}
static TuriValue native_sbuf_max_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]);
    if (!b || b->len == 0) { fprintf(stderr, "sized-buf-max: empty buffer\n"); _exit(1); return turi_int(0); }
    int64_t m = b->data[0];
    for (int64_t i = 1; i < b->len; i++) if (b->data[i] > m) m = b->data[i];
    return turi_int(m);
}

/* -------------------------------------------------------------------------
 * MutableMap (stdlib/mutmap.tur) -- open-addressing hash table.  The module's
 * inline-C ops loop and fat-dispatch the value comparator, which the simple
 * inline-C executor cannot run, so these natives re-implement them over the
 * exact same self-contained layout (no runtime HAMT dependency): the wrapper is
 * { void *storage } and storage is { cap, len, tomb, slots[] } with each slot
 * { tag, hash, key, value }.  Only mutmap-eq? needs the interpreter (it invokes
 * the value comparator -- a turi closure -- via turi_call).
 * ---------------------------------------------------------------------- */
enum { TUR_MM_EMPTY = 0, TUR_MM_OCCUPIED = 1, TUR_MM_DELETED = 2 };
typedef struct { uint8_t tag; int64_t hash; int64_t key; int64_t value; } TurMmSlot;
typedef struct { uint64_t cap; uint64_t len; uint64_t tomb; TurMmSlot slots[]; } TurMmStorage;
typedef struct { void *storage; } TurMmWrap;

static TurMmStorage *mm_storage(TuriValue v) {
    TurMmWrap *m = (TurMmWrap *)(intptr_t)v.as_int;
    return m ? (TurMmStorage *)m->storage : NULL;
}
static TurMmStorage *mm_alloc(uint64_t cap) {
    TurMmStorage *s = (TurMmStorage *)malloc(sizeof(*s) + cap * sizeof(TurMmSlot));
    if (!s) return NULL;
    s->cap = cap; s->len = 0; s->tomb = 0;
    for (uint64_t i = 0; i < cap; i++) {
        s->slots[i].tag = TUR_MM_EMPTY;
        s->slots[i].hash = 0; s->slots[i].key = 0; s->slots[i].value = 0;
    }
    return s;
}
static TuriValue native_mutmap_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    TurMmStorage *s = mm_alloc(16);
    if (!s) return turi_nil();
    TurMmWrap *m = (TurMmWrap *)malloc(sizeof(*m));
    if (!m) { free(s); return turi_nil(); }
    m->storage = s;
    return turi_int((int64_t)(intptr_t)m);
}
static TuriValue native_mutmap_len(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    return turi_int(s ? (int64_t)s->len : 0);
}
static TuriValue native_mutmap_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 4) return turi_nil();
    TurMmWrap *m = (TurMmWrap *)(intptr_t)a[0].as_int;
    if (!m) return turi_nil();
    TurMmStorage *s = (TurMmStorage *)m->storage;
    int64_t h = a[1].as_int, key = a[2].as_int, val = a[3].as_int;
    /* Resize when load factor (len + tomb) / cap >= 0.75. */
    if ((s->len + s->tomb) * 4 >= s->cap * 3) {
        TurMmStorage *ns = mm_alloc(s->cap * 2);
        if (!ns) return turi_nil();
        uint64_t mask = ns->cap - 1;
        for (uint64_t i = 0; i < s->cap; i++) {
            if (s->slots[i].tag != TUR_MM_OCCUPIED) continue;
            uint64_t idx = ((uint64_t)s->slots[i].hash) & mask;
            while (ns->slots[idx].tag == TUR_MM_OCCUPIED) idx = (idx + 1) & mask;
            ns->slots[idx] = s->slots[i];
            ns->len++;
        }
        free(s);
        m->storage = ns;
        s = ns;
    }
    uint64_t mask = s->cap - 1;
    uint64_t idx = ((uint64_t)h) & mask;
    int64_t first_tomb = -1;
    for (;;) {
        TurMmSlot *slot = &s->slots[idx];
        if (slot->tag == TUR_MM_EMPTY) {
            TurMmSlot *dst = (first_tomb >= 0) ? &s->slots[first_tomb] : slot;
            dst->tag = TUR_MM_OCCUPIED; dst->hash = h; dst->key = key; dst->value = val;
            if (first_tomb >= 0) s->tomb--;
            s->len++;
            return turi_nil();
        }
        if (slot->tag == TUR_MM_OCCUPIED && slot->hash == h && slot->key == key) {
            slot->value = val;
            return turi_nil();
        }
        if (slot->tag == TUR_MM_DELETED && first_tomb < 0) first_tomb = (int64_t)idx;
        idx = (idx + 1) & mask;
    }
}
static TuriValue native_mutmap_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_int(0);
    int64_t h = a[1].as_int, key = a[2].as_int;
    uint64_t mask = s->cap - 1, idx = ((uint64_t)h) & mask;
    for (;;) {
        TurMmSlot *slot = &s->slots[idx];
        if (slot->tag == TUR_MM_EMPTY) return turi_int(0);
        if (slot->tag == TUR_MM_OCCUPIED && slot->hash == h && slot->key == key)
            return turi_int(slot->value);
        idx = (idx + 1) & mask;
    }
}
static TuriValue native_mutmap_has(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_bool(false);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_bool(false);
    int64_t h = a[1].as_int, key = a[2].as_int;
    uint64_t mask = s->cap - 1, idx = ((uint64_t)h) & mask;
    for (;;) {
        TurMmSlot *slot = &s->slots[idx];
        if (slot->tag == TUR_MM_EMPTY) return turi_bool(false);
        if (slot->tag == TUR_MM_OCCUPIED && slot->hash == h && slot->key == key)
            return turi_bool(true);
        idx = (idx + 1) & mask;
    }
}
static TuriValue native_mutmap_delete(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_bool(false);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_bool(false);
    int64_t h = a[1].as_int, key = a[2].as_int;
    uint64_t mask = s->cap - 1, idx = ((uint64_t)h) & mask;
    for (;;) {
        TurMmSlot *slot = &s->slots[idx];
        if (slot->tag == TUR_MM_EMPTY) return turi_bool(false);
        if (slot->tag == TUR_MM_OCCUPIED && slot->hash == h && slot->key == key) {
            slot->tag = TUR_MM_DELETED; slot->hash = 0; slot->key = 0; slot->value = 0;
            s->len--; s->tomb++;
            return turi_bool(true);
        }
        idx = (idx + 1) & mask;
    }
}
/* mutmap-cap / mutmap-slot-* -- the slot-iteration accessors the pure-Turmeric
 * `mutmap-eq-loop` (stdlib/mutmap.tur) walks.  They replace the old inline-C
 * `mutmap-eq-storage?` carrier core: equality is now expressed in Turmeric over
 * these reads plus mutmap-has?/mutmap-get, and the value comparator (a turi
 * closure) is invoked directly by the interpreted loop -- no native needs to
 * call back into a comparator.  Each is a single bounds-checked field read. */
static TuriValue native_mutmap_cap(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    return turi_int(s ? (int64_t)s->cap : 0);
}
static TuriValue native_mutmap_slot_occupied(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_bool(false);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_bool(false);
    int64_t i = a[1].as_int;
    if (i < 0 || (uint64_t)i >= s->cap) return turi_bool(false);
    return turi_bool(s->slots[i].tag == TUR_MM_OCCUPIED);
}
static TuriValue native_mutmap_slot_hash(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_int(0);
    int64_t i = a[1].as_int;
    if (i < 0 || (uint64_t)i >= s->cap) return turi_int(0);
    return turi_int(s->slots[i].hash);
}
static TuriValue native_mutmap_slot_key(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_int(0);
    int64_t i = a[1].as_int;
    if (i < 0 || (uint64_t)i >= s->cap) return turi_int(0);
    return turi_int(s->slots[i].key);
}
static TuriValue native_mutmap_slot_value(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_int(0);
    int64_t i = a[1].as_int;
    if (i < 0 || (uint64_t)i >= s->cap) return turi_int(0);
    return turi_int(s->slots[i].value);
}
/* mutmap-storage-field__ -- read the raw `storage` pointer out of a carrier
 * MutableMap wrapper (the inline-C bridge the public mutmap-eq? uses to feed
 * the by-value storage core).  The interpreter holds the wrapper as a TURI_INT
 * pointer to { void *storage }; return word 0 as a TURI_INT ptr<void>. */
static TuriValue native_mutmap_storage_field(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || a[0].tag != TURI_INT || a[0].as_int == 0) return turi_int(0);
    TurMmWrap *m = (TurMmWrap *)(intptr_t)a[0].as_int;
    return turi_int((int64_t)(intptr_t)m->storage);
}
/* mutmap-eq-storage? -- structural equality over two raw storage pointers,
 * invoking the value comparator (a turi closure) via turi_call.  Native
 * override for the inline-C core that the public mutmap-eq? wrapper delegates
 * to (the Eq[MutableMap] instance walks the pure-Turmeric mutmap-eq-loop
 * instead, so it never reaches this native). */
static TuriValue native_mutmap_eq_storage(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_bool(false);
    TurMmStorage *sa = (TurMmStorage *)(intptr_t)a[0].as_int;
    TurMmStorage *sb = (TurMmStorage *)(intptr_t)a[1].as_int;
    TuriValue cmp = a[2];
    if (!sa || !sb || sa->len != sb->len) return turi_bool(false);
    for (uint64_t i = 0; i < sa->cap; i++) {
        if (sa->slots[i].tag != TUR_MM_OCCUPIED) continue;
        int64_t h = sa->slots[i].hash, k = sa->slots[i].key, v_a = sa->slots[i].value;
        uint64_t mask = sb->cap - 1, idx = ((uint64_t)h) & mask;
        bool found = false; int64_t v_b = 0;
        for (;;) {
            TurMmSlot *slot = &sb->slots[idx];
            if (slot->tag == TUR_MM_EMPTY) break;
            if (slot->tag == TUR_MM_OCCUPIED && slot->hash == h && slot->key == k) {
                v_b = slot->value; found = true; break;
            }
            idx = (idx + 1) & mask;
        }
        if (!found) return turi_bool(false);
        TuriValue cargs[2];
        cargs[0].tag = TURI_INT; cargs[0].as_int = v_a;
        cargs[1].tag = TURI_INT; cargs[1].as_int = v_b;
        TuriValue rv = turi_call(env, cmp, cargs, 2);
        if (turi_is_error(rv) || env->throwing) return rv;  /* propagate callback error */
        if (!(rv.tag == TURI_BOOL ? rv.as_bool : rv.as_int != 0)) return turi_bool(false);
    }
    return turi_bool(true);
}
static TuriValue native_mutmap_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || a[0].tag != TURI_INT || a[0].as_int == 0) return turi_nil();
    TurMmWrap *m = (TurMmWrap *)(intptr_t)a[0].as_int;
    if (m->storage) free(m->storage);
    free(m);
    return turi_nil();
}

/* nil-value: return 0 (empty list sentinel) */
static TuriValue native_nil_value(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    return turi_int(0);
}
/* cons: allocate a new cons cell {value, next} and return pointer as int64 */
static TuriValue native_cons(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t value = (n > 0) ? a[0].as_int : 0;
    int64_t next  = (n > 1) ? a[1].as_int : 0;
    int64_t *cell = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!cell) return turi_nil();
    cell[0] = value; cell[1] = next;
    return turi_int((int64_t)(intptr_t)cell);
}
/* tail: return the next pointer field of the first cons cell.  Handles both the
 * interpreter struct representation (tcons -> make-struct Cons; tail is field 1)
 * and the carrier { head, tail } box (untyped cons surface). */
static TuriValue native_list_tail(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    if (a[0].tag == TURI_STRUCT) {
        bool f = false; TuriValue t = turi_struct_field(a[0], 1, &f);
        return f ? t : turi_int(0);
    }
    if (a[0].as_int == 0) return turi_int(0);
    int64_t *cell = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(cell[1]);
}
/* list-nil?: true if the cons-cell pointer is 0 (empty list) */
static TuriValue native_list_nil_pred(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    TuriValue rv = {0}; rv.tag = TURI_BOOL;
    rv.as_bool = (n == 0 || a[0].as_int == 0);
    return rv;
}
/* head: return the value field of the first cons cell.  Handles both the
 * interpreter struct representation (tcons -> make-struct Cons; head is field 0,
 * which may carry any TuriValue) and the carrier { head, tail } box. */
static TuriValue native_list_head(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    if (a[0].tag == TURI_STRUCT) {
        bool f = false; TuriValue h = turi_struct_field(a[0], 0, &f);
        return f ? h : turi_nil();
    }
    if (a[0].as_int == 0) return turi_nil();
    int64_t *cell = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(cell[0]);
}
/* cstr->parse-int: parse a raw int (cstr pointer as int64) to int64 */
static TuriValue native_cstr_parse_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    const char *s = (const char *)(intptr_t)a[0].as_int;
    return turi_int(s ? (int64_t)atoll(s) : 0);
}
/* bit-shr: logical (unsigned) right shift */
static TuriValue native_bit_shr(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    return turi_int((int64_t)((uint64_t)a[0].as_int >> (unsigned)a[1].as_int));
}
/* bit-xor: bitwise XOR of two integers */
static TuriValue native_bit_xor(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    return turi_int(a[0].as_int ^ a[1].as_int);
}
/* println-float: print float with given decimal places */
static TuriValue native_println_float(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? (a[0].tag == TURI_FLOAT ? a[0].as_float : (double)a[0].as_int) : 0.0;
    int d = (n > 1) ? (int)a[1].as_int : 6;
    if (d < 0) d = 0;
    if (d > 17) d = 17;
    char fmt[16];
    snprintf(fmt, sizeof(fmt), "%%.%df\n", d);
    printf(fmt, x);
    return turi_nil();
}
/* int->unit-float: map a 64-bit int to [0,1) by dividing by 2^53 */
static TuriValue native_int_to_unit_float(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double v = (n > 0) ? (double)(uint64_t)a[0].as_int / 9007199254740992.0 : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = v; return rv;
}
/* tur-sqrt: square root via libm */
static TuriValue native_tur_sqrt(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0 && a[0].tag == TURI_FLOAT) ? a[0].as_float : (n > 0 ? (double)a[0].as_int : 0.0);
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = sqrt(x); return rv;
}
/* int->float: cast int64 to double */
static TuriValue native_int_to_float(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double v = (n > 0) ? (double)a[0].as_int : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = v; return rv;
}
/* float->int: truncate toward zero (math.tur's inline-C `(int64_t)x`). */
static TuriValue native_float_to_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double v = (n > 0) ? a[0].as_float : 0.0;
    return turi_int((int64_t)v);
}
/* sqrt / floor: math.tur's libm wrappers (inline-C the tree-walker cannot run). */
static TuriValue native_math_sqrt(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? a[0].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = sqrt(x); return rv;
}
static TuriValue native_math_floor(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? a[0].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = floor(x); return rv;
}
/* N2 (numeric-tower-rational-complex-plan): the transcendental libm wrappers
 * stdlib/complex.tur builds `complex/exp` and `complex/arg` on.  They are
 * inline-C in math.tur, which the tree-walker cannot run, so each needs the
 * same native override sqrt/floor already carry -- otherwise a Complex program
 * would diverge between the compiled and interpreted engines. */
static TuriValue native_math_exp(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? a[0].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = exp(x); return rv;
}
static TuriValue native_math_log(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? a[0].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = log(x); return rv;
}
static TuriValue native_math_sin(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? a[0].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = sin(x); return rv;
}
static TuriValue native_math_cos(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? a[0].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = cos(x); return rv;
}
static TuriValue native_math_atan2(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double y = (n > 0) ? a[0].as_float : 0.0;
    double x = (n > 1) ? a[1].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = atan2(y, x); return rv;
}
/* fabs / ceil / pow: math.tur's remaining libm wrappers, which had no native
 * override.  stdlib/complex.tur reaches `fabs` from complex/div and complex/abs,
 * so without these an interpreted Complex program dies on "inline-C not
 * supported in interpreter mode" the moment it divides. */
static TuriValue native_math_fabs(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? a[0].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = fabs(x); return rv;
}
static TuriValue native_math_ceil(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? a[0].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = ceil(x); return rv;
}
static TuriValue native_math_pow(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? a[0].as_float : 0.0;
    double y = (n > 1) ? a[1].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = pow(x, y); return rv;
}

/* -------------------------------------------------------------------------
 * I/O benchmark native helpers (file_read.tur, file_write.tur).
 *
 * These replace the inline-C helper definitions in the turmeric/ benchmark
 * files so the shared turi/ symlinks work under tur --interpret.
 * ---------------------------------------------------------------------- */

/* Helper: extract a C-string from a TuriValue (TURI_CSTR or TURI_INT ptr). */
static const char *tv_to_cstr(TuriValue v) {
    if (v.tag == TURI_CSTR) return v.as_cstr;
    return (const char *)(intptr_t)v.as_int;
}

/* write-temp-file [path :cstr n :int] :nil -- write n bytes to path. */
static TuriValue native_write_temp_file(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    const char *path = tv_to_cstr(a[0]);
    int64_t bytes = a[1].as_int;
    if (!path || bytes <= 0) return turi_nil();
    char buf[4096];
    memset(buf, 0xCD, sizeof(buf));
    FILE *f = fopen(path, "wb");
    if (!f) return turi_nil();
    int64_t rem = bytes;
    while (rem > 0) {
        int64_t chunk = rem < 4096 ? rem : 4096;
        fwrite(buf, 1, (size_t)chunk, f);
        rem -= chunk;
    }
    fclose(f);
    return turi_nil();
}

/* io-fopen-read [path :cstr] :int -- fopen "rb", return FILE* as int64. */
static TuriValue native_io_fopen_read(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *path = (n >= 1) ? tv_to_cstr(a[0]) : NULL;
    FILE *f = path ? fopen(path, "rb") : NULL;
    return turi_int((int64_t)(intptr_t)f);
}

/* io-fread-chunk [fp :int buf :int] :int -- fread up to 4096 bytes; return bytes read. */
static TuriValue native_io_fread_chunk(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    FILE *fp  = (FILE *)(intptr_t)a[0].as_int;
    void *buf = (void *)(intptr_t)a[1].as_int;
    if (!fp || !buf) return turi_int(0);
    return turi_int((int64_t)fread(buf, 1, 4096, fp));
}

/* io-fclose [fp :int] :nil -- fclose a FILE*. */
static TuriValue native_io_fclose(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1 && a[0].as_int) fclose((FILE *)(intptr_t)a[0].as_int);
    return turi_nil();
}

/* io-remove [path :cstr] :nil -- remove a file. */
static TuriValue native_io_remove(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *path = (n >= 1) ? tv_to_cstr(a[0]) : NULL;
    if (path) remove(path);
    return turi_nil();
}

/* io-buf-new [] :int -- malloc 4096 bytes; return pointer as int64. */
static TuriValue native_io_buf_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)malloc(4096));
}

/* io-buf-free [buf :int] :nil -- free a buffer. */
static TuriValue native_io_buf_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1 && a[0].as_int) free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}

/* io-alloc [n :int v :int] :int -- malloc n bytes filled with byte v. */
static TuriValue native_io_alloc(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    size_t sz  = (n > 0 && a[0].as_int > 0) ? (size_t)a[0].as_int : 0;
    int    val = (n > 1) ? (int)a[1].as_int : 0;
    void *buf = sz ? malloc(sz) : NULL;
    if (buf) memset(buf, val, sz);
    return turi_int((int64_t)(intptr_t)buf);
}

/* io-free [buf :int] :nil -- free an io-alloc'd buffer. */
static TuriValue native_io_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1 && a[0].as_int) free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}

/* io-fopen-write [path :cstr] :int -- fopen "wb", return FILE* as int64. */
static TuriValue native_io_fopen_write(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *path = (n >= 1) ? tv_to_cstr(a[0]) : NULL;
    FILE *f = path ? fopen(path, "wb") : NULL;
    return turi_int((int64_t)(intptr_t)f);
}

/* io-fwrite-chunk [fp :int buf :int offset :int chunk :int] :int -- fwrite; return bytes. */
static TuriValue native_io_fwrite_chunk(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) return turi_int(0);
    FILE *fp    = (FILE *)(intptr_t)a[0].as_int;
    char *buf   = (char *)(intptr_t)a[1].as_int;
    int64_t off = a[2].as_int;
    int64_t len = a[3].as_int;
    if (!fp || !buf || len <= 0) return turi_int(0);
    return turi_int((int64_t)fwrite(buf + off, 1, (size_t)len, fp));
}

/* -------------------------------------------------------------------------
 * Whole-benchmark native implementations for benchmarks whose logic is
 * written entirely in inline-C (legitimate platform I/O or concurrency tests).
 * ---------------------------------------------------------------------- */

/* random-access-bench [file_size :int n_reads :int] :int */
static TuriValue native_random_access_bench(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t file_size = a[0].as_int;
    int64_t n_reads   = a[1].as_int;
    const char *path  = "/tmp/bench_io_random_tur.bin";
    char wbuf[4096];
    FILE *fw = fopen(path, "wb");
    if (!fw) return turi_int(-1);
    int64_t rem = file_size;
    int seq = 0;
    while (rem > 0) {
        int64_t chunk = rem < 4096 ? rem : 4096;
        for (int64_t i = 0; i < chunk; i++) wbuf[i] = (char)(seq++ & 0xFF);
        fwrite(wbuf, 1, (size_t)chunk, fw);
        rem -= chunk;
    }
    fclose(fw);
    FILE *fr = fopen(path, "rb");
    if (!fr) return turi_int(-1);
    uint64_t state = 12345678ULL;
    int64_t  checksum = 0;
    unsigned char byte;
    for (int64_t i = 0; i < n_reads; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t offset = (int64_t)(state >> 1) % file_size;
        fseek(fr, (long)offset, SEEK_SET);
        if (fread(&byte, 1, 1, fr)) checksum += byte;
    }
    fclose(fr);
    remove(path);
    return turi_int(checksum);
}

/* ring_worker_nat: pthread worker for the thread-ring benchmark. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             ready;
    int64_t         token;
} TRSlot_nat;
typedef struct { TRSlot_nat *ring; int id; int n; } TRArg_nat;
static void *ring_worker_nat(void *vp) {
    TRArg_nat *a = (TRArg_nat *)vp;
    TRSlot_nat *me   = &a->ring[a->id];
    TRSlot_nat *next = &a->ring[(a->id + 1) % a->n];
    while (1) {
        pthread_mutex_lock(&me->mu);
        while (!me->ready) pthread_cond_wait(&me->cv, &me->mu);
        int64_t tok = me->token; me->ready = 0;
        pthread_mutex_unlock(&me->mu);
        int64_t out = tok > 0 ? tok - 1 : tok;
        pthread_mutex_lock(&next->mu);
        next->token = out; next->ready = 1;
        pthread_cond_signal(&next->cv);
        pthread_mutex_unlock(&next->mu);
        if (tok <= 0) return NULL;
    }
}

/* run-ring [n_threads :int messages :int] :nil */
static TuriValue native_run_ring(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    int n_threads = (int)a[0].as_int;
    int messages  = (int)a[1].as_int;
    if (n_threads <= 0) return turi_nil();
    TRSlot_nat *ring = (TRSlot_nat *)calloc((size_t)n_threads, sizeof(TRSlot_nat));
    for (int i = 0; i < n_threads; i++) {
        pthread_mutex_init(&ring[i].mu, NULL);
        pthread_cond_init(&ring[i].cv, NULL);
    }
    TRArg_nat *targs = (TRArg_nat *)malloc((size_t)n_threads * sizeof(TRArg_nat));
    for (int i = 0; i < n_threads; i++) {
        targs[i].ring = ring; targs[i].id = i; targs[i].n = n_threads;
    }
    pthread_t *threads = (pthread_t *)malloc((size_t)n_threads * sizeof(pthread_t));
    for (int i = 0; i < n_threads; i++)
        pthread_create(&threads[i], NULL, ring_worker_nat, &targs[i]);
    pthread_mutex_lock(&ring[0].mu);
    ring[0].token = messages; ring[0].ready = 1;
    pthread_cond_signal(&ring[0].cv);
    pthread_mutex_unlock(&ring[0].mu);
    for (int i = 0; i < n_threads; i++) pthread_join(threads[i], NULL);
    printf("done\n");
    free(threads); free(targs); free(ring);
    return turi_nil();
}

/* run-nbody [n_bodies :int steps :int] :nil */
static TuriValue native_run_nbody(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    int n_bodies = (int)a[0].as_int;
    int steps    = (int)a[1].as_int;
    if (n_bodies <= 0) return turi_nil();
    typedef struct { double x,y,z,vx,vy,vz,mass; } NBody_nat;
    NBody_nat *b = (NBody_nat *)calloc((size_t)n_bodies, sizeof(NBody_nat));
    uint64_t state = 42;
    for (int i = 0; i < n_bodies; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        b[i].x = (double)(int64_t)(state >> 32) / 1e8;
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        b[i].y = (double)(int64_t)(state >> 32) / 1e8;
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        b[i].z = (double)(int64_t)(state >> 32) / 1e8;
        b[i].vx = b[i].vy = b[i].vz = 0.0;
        b[i].mass = 1.0 + (i % 5) * 0.5;
    }
    for (int s = 0; s < steps; s++) {
        for (int i = 0; i < n_bodies; i++)
            for (int j = i + 1; j < n_bodies; j++) {
                double dx = b[j].x-b[i].x, dy = b[j].y-b[i].y, dz = b[j].z-b[i].z;
                double dist = sqrt(dx*dx+dy*dy+dz*dz) + 1e-10;
                double f = b[i].mass * b[j].mass / (dist*dist*dist);
                b[i].vx+=f*dx; b[i].vy+=f*dy; b[i].vz+=f*dz;
                b[j].vx-=f*dx; b[j].vy-=f*dy; b[j].vz-=f*dz;
            }
        for (int i = 0; i < n_bodies; i++) {
            b[i].x += b[i].vx; b[i].y += b[i].vy; b[i].z += b[i].vz;
        }
    }
    double ke = 0;
    for (int i = 0; i < n_bodies; i++)
        ke += 0.5 * b[i].mass * (b[i].vx*b[i].vx + b[i].vy*b[i].vy + b[i].vz*b[i].vz);
    printf("%.4f\n", ke);
    free(b);
    return turi_nil();
}

/* run-raytracer [width :int height :int] :int */
static TuriValue native_run_raytracer(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t width  = a[0].as_int;
    int64_t height = a[1].as_int;
    typedef struct { double x, y, z; } RT_Vec3;
    typedef struct { RT_Vec3 center; double radius; } RT_Sphere;
#define RT_ADD(a,b) ((RT_Vec3){(a).x+(b).x,(a).y+(b).y,(a).z+(b).z})
#define RT_SUB(a,b) ((RT_Vec3){(a).x-(b).x,(a).y-(b).y,(a).z-(b).z})
#define RT_SCALE(v,s) ((RT_Vec3){(v).x*(s),(v).y*(s),(v).z*(s)})
#define RT_DOT(a,b) ((a).x*(b).x+(a).y*(b).y+(a).z*(b).z)
    RT_Sphere spheres[] = {{{0,0,-5},1.0}, {{2,0,-7},1.5}, {{-3,0,-6},0.8}};
    RT_Vec3 lraw = {1, 1, -1};
    double llen = sqrt(RT_DOT(lraw, lraw)) + 1e-15;
    RT_Vec3 light = RT_SCALE(lraw, 1.0 / llen);
    RT_Vec3 origin = {0, 0, 0};
    int64_t checksum = 0;
    for (int64_t y = 0; y < height; y++) {
        for (int64_t x = 0; x < width; x++) {
            double u = ((double)x / width) * 2 - 1;
            double v = ((double)y / height) * 2 - 1;
            RT_Vec3 dv = {u, v, -1};
            double dlen = sqrt(RT_DOT(dv, dv)) + 1e-15;
            RT_Vec3 dir = RT_SCALE(dv, 1.0 / dlen);
            double best = 1e18; int bi = -1;
            for (int i = 0; i < 3; i++) {
                RT_Vec3 oc = RT_SUB(origin, spheres[i].center);
                double aa = RT_DOT(dir, dir), b2 = RT_DOT(oc, dir);
                double c = RT_DOT(oc, oc) - spheres[i].radius * spheres[i].radius;
                double d = b2*b2 - aa*c;
                if (d >= 0) {
                    double t = (-b2 - sqrt(d)) / aa;
                    if (t > 0.001 && t < best) { best = t; bi = i; }
                }
            }
            if (bi >= 0) {
                RT_Vec3 hp = RT_ADD(origin, RT_SCALE(dir, best));
                RT_Vec3 nv = RT_SUB(hp, spheres[bi].center);
                double nlen2 = sqrt(RT_DOT(nv, nv)) + 1e-15;
                RT_Vec3 norm = RT_SCALE(nv, 1.0 / nlen2);
                double diff = RT_DOT(norm, light);
                if (diff < 0) diff = 0;
                checksum += (int64_t)(diff * 255);
            }
        }
    }
#undef RT_ADD
#undef RT_SUB
#undef RT_SCALE
#undef RT_DOT
    return turi_int(checksum);
}

/* stdlib/time.tur natives.  time.tur's bodies are inline-C (nanosleep, a
 * __tur_mock_cap singleton struct), which the interpreter cannot run.  The
 * Mock-Time capability is the only deterministic, printable clock; model it as
 * a one-word heap cell holding the current mock time (ms).  sleep-ms is a no-op
 * under --interpret (deterministic; the interpreter has no wall clock to
 * advance), matching the fixtures that only assert on mock time. */
static TuriValue native_time_mock_create(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    int64_t *cell = (int64_t *)malloc(sizeof(int64_t));
    if (!cell) return turi_int(0);
    cell[0] = 0;
    return turi_int((int64_t)(intptr_t)cell);
}
static TuriValue native_time_mock_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].as_int) free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_time_mock_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n >= 2 && a[0].as_int) ((int64_t *)(intptr_t)a[0].as_int)[0] = a[1].as_int;
    return turi_nil();
}
static TuriValue native_time_mock_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n >= 1 && a[0].as_int) return turi_int(((int64_t *)(intptr_t)a[0].as_int)[0]);
    return turi_int(0);
}
static TuriValue native_time_sleep_ms(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    return turi_nil();  /* deterministic no-op under --interpret */
}

/* stdlib/reactor.tur: the Reactor handle ops are thin shims over the tur_reactor_*
 * runtime (src/async/reactor.c) -- reactor-new's inline-C body and reactor-free/
 * reactor-poll's extern-c calls.  The tree-walker has no extern-c symbol table
 * and cannot run the inline-C, so bind the runtime entry points by their C names
 * (and reactor-new by name, overriding its inline-C body).  The handle is a
 * pointer carried as int64, matching the compiled :ptr<void> carrier. */
extern void *tur_reactor_new(void);
extern void  tur_reactor_free(void *r);
extern int64_t tur_reactor_poll(void *r, int64_t timeout_ms);
static TuriValue native_reactor_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_reactor_new());
}
static TuriValue native_reactor_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1 && a[0].as_int) tur_reactor_free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_reactor_poll(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2 || a[0].as_int == 0) return turi_int(0);
    return turi_int(tur_reactor_poll((void *)(intptr_t)a[0].as_int, a[1].as_int));
}

void wk_register_stdlib_natives(TuriEnv *env) {
    /* stdlib/time.tur (inline-C; Mock-Time capability + sleep-ms) */
    turi_env_register_native(env, "Mock-Time",       native_time_mock_create, NULL);
    turi_env_register_native(env, "Mock-Time-free",  native_time_mock_free,   NULL);
    turi_env_register_native(env, "mock-time-set",   native_time_mock_set,    NULL);
    turi_env_register_native(env, "mock-time-get",   native_time_mock_get,    NULL);
    turi_env_register_native(env, "sleep-ms",        native_time_sleep_ms,    NULL);
    /* Option/some/none */
    turi_env_register_native(env, "some",            native_some,            NULL);
    turi_env_register_native(env, "option-eq?",      native_option_eq,       NULL);
    turi_env_register_native(env, "none",            native_none,            NULL);
    turi_env_register_native(env, "some?",           native_some_pred,       NULL);
    turi_env_register_native(env, "option-unwrap",   native_option_unwrap,   NULL);
    turi_env_register_native(env, "option-value",    native_option_value,    NULL);
    turi_env_register_native(env, "option-free",     native_option_free,     NULL);
    turi_env_register_native(env, "option-unwrap-or",native_option_unwrap_or,NULL);
    /* option.tur names this inline-C op `unwrap-or` (not `option-unwrap-or`); the
     * override only fires when registered under the real function name. */
    turi_env_register_native(env, "unwrap-or",       native_option_unwrap_or,NULL);
    /* unwrap-or-carrier (option.tur Track A shim) is inline-C with identical
     * semantics to unwrap-or: extract the carrier Option's value or return the
     * default.  Register the same native so --interpret does not trip over the
     * inline-C body. */
    turi_env_register_native(env, "unwrap-or-carrier",native_option_unwrap_or,NULL);
    turi_env_register_native(env, "option-must",     native_option_must,     NULL);
    turi_env_register_native(env, "option-expect",   native_option_expect,   NULL);
    turi_env_register_native(env, "option-map",      native_option_map,      NULL);
    /* kleisli.tur: fat-closure carrier apply (TUR_APPLY1) -> turi_call. */
    turi_env_register_native(env, "k-apply-raw",     native_k_apply_raw,     NULL);
    /* Result/ok/err */
    turi_env_register_native(env, "ok",              native_ok,              NULL);
    turi_env_register_native(env, "err",             native_err,             NULL);
    turi_env_register_native(env, "ok?",             native_ok_pred,         NULL);
    turi_env_register_native(env, "err?",            native_err_pred,        NULL);
    turi_env_register_native(env, "ok-val",          native_ok_val,          NULL);
    turi_env_register_native(env, "err-val",         native_err_val,         NULL);
    turi_env_register_native(env, "result-unwrap",   native_result_unwrap,   NULL);
    turi_env_register_native(env, "result-unwrap-or",native_result_unwrap_or,NULL);
    turi_env_register_native(env, "result-unwrap-err",native_result_unwrap_err,NULL);
    turi_env_register_native(env, "ok-val-ptr",      native_ok_val_ptr,      NULL);
    turi_env_register_native(env, "err-val-ptr",     native_err_val_ptr,     NULL);
    turi_env_register_native(env, "result-map",      native_result_map,      NULL);
    turi_env_register_native(env, "result-eq?",      native_result_eq,       NULL);
    turi_env_register_native(env, "result-map-err",  native_result_map_err,  NULL);
    turi_env_register_native(env, "result-flat-map", native_result_flat_map, NULL);
    turi_env_register_native(env, "result-or",       native_result_or,       NULL);
    turi_env_register_native(env, "result-or-else",  native_result_or_else,  NULL);
    turi_env_register_native(env, "result-display",  native_result_display,  NULL);
    turi_env_register_native(env, "result-debug",    native_result_debug,    NULL);
    turi_env_register_native(env, "result-error-message", native_result_error_message, NULL);
    turi_env_register_native(env, "result-free",     native_result_free,     NULL);
    turi_env_register_native(env, "result-must",     native_result_must,     NULL);
    turi_env_register_native(env, "result-must-msg", native_result_must_msg, NULL);
    turi_env_register_native(env, "result-expect",   native_result_must_msg, NULL);
    /* String conversion */
    turi_env_register_native(env, "int->str",        native_int_to_str,      NULL);
    turi_env_register_native(env, "str->int",        native_str_to_int,      NULL);
    turi_env_register_native(env, "strcmp",          native_strcmp_fn,       NULL);
    /* Layout-exact string builders (interp-string-natives-and-range-show-plan
     * Phase 0): shims over the inline-C str-concat / cstr-len / cstr-nth the
     * tree-walker cannot run. Same binding names, same NUL-terminated cstr ABI.
     * cstr-sub (the substring builder the pure-Turmeric re.tur engine reads
     * output through) rides the same shim cluster. */
    turi_env_register_native(env, "str-concat",      native_str_concat,      NULL);
    turi_env_register_native(env, "cstr-len",        native_cstr_len,        NULL);
    turi_env_register_native(env, "cstr-nth",        native_cstr_nth,        NULL);
    turi_env_register_native(env, "cstr-sub",        native_cstr_sub,        NULL);
    /* Common math/array fixture helpers */
    turi_env_register_native(env, "c-abs",           native_c_abs,           NULL);
    turi_env_register_native(env, "popcount",        native_popcount,        NULL);
    turi_env_register_native(env, "flat-new",        native_flat_new,        NULL);
    turi_env_register_native(env, "flat-get",        native_flat_get,        NULL);
    turi_env_register_native(env, "flat-set",        native_flat_set,        NULL);
    /* Slice operations */
    turi_env_register_native(env, "slice-new",       native_slice_new,       NULL);
    turi_env_register_native(env, "slice-len",       native_slice_len,       NULL);
    turi_env_register_native(env, "slice-get",       native_slice_get,       NULL);
    turi_env_register_native(env, "slice-free",      native_slice_free,      NULL);
    /* Common fixture helpers: int-val, alloc-int, alloc-key, alloc-str, ptr= */
    turi_env_register_native(env, "int-val",         native_int_val,         NULL);
    turi_env_register_native(env, "alloc-int",       native_alloc_int,       NULL);
    turi_env_register_native(env, "alloc-key",       native_alloc_int,       NULL);
    turi_env_register_native(env, "alloc-str",       native_alloc_str,       NULL);
    turi_env_register_native(env, "cstr-free",       native_cstr_free,       NULL);
    turi_env_register_native(env, "ptr=",            native_ptr_eq,          NULL);
    /* Typed-list (list.tur) carrier-level ops.  list-head / list-tail reuse the
     * existing native_list_head / native_list_tail (which handle BOTH a real
     * TuriStruct Cons and the { head, tail } carrier box); list-length is new.
     *
     * tcons is deliberately NOT overridden: its stdlib body is a plain
     * `(make-struct Cons ...)` the interpreter runs directly, producing a
     * TURI_STRUCT -- identical to tcons-of and to what the `list` macro builds.
     * Shadowing it with native_cons (a malloc'd carrier box) made list-concat,
     * which returns a struct-chain l2 unchanged at its base case and then wraps
     * it with tcons, splice a struct value's bits into a carrier tail slot; a
     * later list-length walked that garbage pointer and crashed.  Letting tcons
     * resolve to its defn keeps every constructor on one representation. */
    turi_env_register_native(env, "list-head",       native_list_head,       NULL);
    turi_env_register_native(env, "list-tail",       native_list_tail,       NULL);
    turi_env_register_native(env, "list-length",     native_list_length,     NULL);
    /* stdlib/reactor.tur handle ops (inline-C / extern-c over tur_reactor_*). */
    turi_env_register_native(env, "reactor-new",      native_reactor_new,    NULL);
    turi_env_register_native(env, "tur_reactor_new",  native_reactor_new,    NULL);
    turi_env_register_native(env, "tur_reactor_free", native_reactor_free,   NULL);
    turi_env_register_native(env, "tur_reactor_poll", native_reactor_poll,   NULL);
    /* Free monad (free.tur): #{Unsafe} inline-C bodies re-implemented over the
     * PureFree/Suspend TuriStruct + turi_call continuation. */
    turi_env_register_native(env, "free-bind",       native_free_bind,       NULL);
    turi_env_register_native(env, "free-run",        native_free_run,        NULL);
    turi_env_register_native(env, "str->int-checked", native_str_to_int_checked, NULL);
    /* Grid (grid.tur): raw-buffer inline-C re-implemented over the matching
     * { data, width, height, cx, cy } header. */
    turi_env_register_native(env, "grid-new",        native_grid_new,        NULL);
    turi_env_register_native(env, "grid-get",        native_grid_get,        NULL);
    turi_env_register_native(env, "grid-set!",       native_grid_set,        NULL);
    turi_env_register_native(env, "grid-width",      native_grid_width,      NULL);
    turi_env_register_native(env, "grid-height",     native_grid_height,     NULL);
    turi_env_register_native(env, "grid-free",       native_grid_free,       NULL);
    /* SizedBuf (sized-buf.tur): __sized-buf-*-raw inline-C over { len, data }. */
    turi_env_register_native(env, "__sized-buf-new-raw",        native_sbuf_new_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-new-zeroed-raw", native_sbuf_new_zeroed_raw, NULL);
    turi_env_register_native(env, "__sized-buf-free-raw",       native_sbuf_free_raw,       NULL);
    turi_env_register_native(env, "__sized-buf-len-raw",        native_sbuf_len_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-get-raw",        native_sbuf_get_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-set!-raw",       native_sbuf_set_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-fill!-raw",      native_sbuf_fill_raw,       NULL);
    turi_env_register_native(env, "__sized-buf-copy!-raw",      native_sbuf_copy_raw,       NULL);
    turi_env_register_native(env, "__sized-buf-sum-raw",        native_sbuf_sum_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-min-raw",        native_sbuf_min_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-max-raw",        native_sbuf_max_raw,        NULL);
    turi_env_register_native(env, "mutmap-new",      native_mutmap_new,      NULL);
    turi_env_register_native(env, "mutmap-len",      native_mutmap_len,      NULL);
    turi_env_register_native(env, "mutmap-set!",     native_mutmap_set,      NULL);
    turi_env_register_native(env, "mutmap-get",      native_mutmap_get,      NULL);
    turi_env_register_native(env, "mutmap-has?",     native_mutmap_has,      NULL);
    turi_env_register_native(env, "mutmap-delete!",  native_mutmap_delete,   NULL);
    /* mutmap-eq? / mutmap-eq-loop are pure-Turmeric; the slot accessors they
     * walk are native (the simple inline-C executor reads them, but registering
     * keeps the interpreter on the same self-contained open-addressing layout). */
    turi_env_register_native(env, "mutmap-cap",            native_mutmap_cap,           NULL);
    turi_env_register_native(env, "mutmap-slot-occupied?", native_mutmap_slot_occupied, NULL);
    turi_env_register_native(env, "mutmap-slot-hash",      native_mutmap_slot_hash,     NULL);
    turi_env_register_native(env, "mutmap-slot-key",       native_mutmap_slot_key,      NULL);
    turi_env_register_native(env, "mutmap-slot-value",     native_mutmap_slot_value,    NULL);
    /* Public mutmap-eq? still delegates to the inline-C storage core. */
    turi_env_register_native(env, "mutmap-storage-field__", native_mutmap_storage_field, NULL);
    turi_env_register_native(env, "mutmap-eq-storage?",     native_mutmap_eq_storage,    NULL);
    turi_env_register_native(env, "mutmap-free",     native_mutmap_free,     NULL);
    turi_env_register_native(env, "result-collect",  native_result_collect,  NULL);
    turi_env_register_native(env, "result-partition",native_result_partition, NULL);
    turi_env_register_native(env, "result-partition-ok", native_result_partition_ok, NULL);
    turi_env_register_native(env, "result-partition-err", native_result_partition_err, NULL);
    /* List operations for benchmark arg parsing and list_ops benchmark */
    turi_env_register_native(env, "nil-value",         native_nil_value,       NULL);
    turi_env_register_native(env, "cons",              native_cons,            NULL);
    turi_env_register_native(env, "list-nil?",         native_list_nil_pred,   NULL);
    turi_env_register_native(env, "head",              native_list_head,       NULL);
    turi_env_register_native(env, "tail",              native_list_tail,       NULL);
    /* Benchmark micro-helpers */
    turi_env_register_native(env, "cstr->parse-int",  native_cstr_parse_int,  NULL);
    turi_env_register_native(env, "bit-shr",           native_bit_shr,         NULL);
    turi_env_register_native(env, "bit-xor",           native_bit_xor,         NULL);
    turi_env_register_native(env, "println-float",     native_println_float,   NULL);
    turi_env_register_native(env, "int->unit-float",   native_int_to_unit_float, NULL);
    turi_env_register_native(env, "tur-sqrt",          native_tur_sqrt,        NULL);
    turi_env_register_native(env, "int->float",        native_int_to_float,    NULL);
    turi_env_register_native(env, "float->int",        native_float_to_int,    NULL);
    turi_env_register_native(env, "sqrt",              native_math_sqrt,       NULL);
    turi_env_register_native(env, "floor",             native_math_floor,      NULL);
    turi_env_register_native(env, "exp",               native_math_exp,        NULL);
    turi_env_register_native(env, "log",               native_math_log,        NULL);
    turi_env_register_native(env, "sin",               native_math_sin,        NULL);
    turi_env_register_native(env, "cos",               native_math_cos,        NULL);
    turi_env_register_native(env, "atan2",             native_math_atan2,      NULL);
    turi_env_register_native(env, "fabs",              native_math_fabs,       NULL);
    turi_env_register_native(env, "ceil",              native_math_ceil,       NULL);
    turi_env_register_native(env, "pow",               native_math_pow,        NULL);
    /* I/O benchmark helpers */
    turi_env_register_native(env, "write-temp-file",   native_write_temp_file, NULL);
    turi_env_register_native(env, "io-fopen-read",     native_io_fopen_read,   NULL);
    turi_env_register_native(env, "io-fread-chunk",    native_io_fread_chunk,  NULL);
    turi_env_register_native(env, "io-fclose",         native_io_fclose,       NULL);
    turi_env_register_native(env, "io-remove",         native_io_remove,       NULL);
    turi_env_register_native(env, "io-buf-new",        native_io_buf_new,      NULL);
    turi_env_register_native(env, "io-buf-free",       native_io_buf_free,     NULL);
    turi_env_register_native(env, "io-alloc",          native_io_alloc,        NULL);
    turi_env_register_native(env, "io-free",           native_io_free,         NULL);
    turi_env_register_native(env, "io-fopen-write",    native_io_fopen_write,  NULL);
    turi_env_register_native(env, "io-fwrite-chunk",   native_io_fwrite_chunk, NULL);
    /* Whole-benchmark natives */
    turi_env_register_native(env, "random-access-bench", native_random_access_bench, NULL);
    turi_env_register_native(env, "run-ring",          native_run_ring,        NULL);
    turi_env_register_native(env, "run-nbody",         native_run_nbody,       NULL);
    turi_env_register_native(env, "run-raytracer",     native_run_raytracer,   NULL);
}

/* -------------------------------------------------------------------------
 * Native implementations of safe.tur stdlib functions (inline-C bodies).
 * These allow safe.tur functions to run correctly in interpreter mode.
 * ---------------------------------------------------------------------- */

/* array-get [arr :ptr<void> idx :int] :ptr
 * Returns a heap-allocated { bool is_some; int64_t value; } option. */
static TuriValue native_safe_array_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_nil();
    int64_t *arr = (int64_t *)(intptr_t)a[0].as_int;
    int64_t  idx = a[1].as_int;
    /* Layout: { bool is_some (8 bytes as int64), int64_t value } */
    int64_t *opt = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!opt) return turi_nil();
    if (arr && idx >= 0 && idx < 1024) {
        opt[0] = 1; /* is_some = true */
        opt[1] = arr[idx];
    } else {
        opt[0] = 0; /* is_some = false */
        opt[1] = 0;
    }
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)opt;
    return v;
}

/* array-set [arr :ptr<void> idx :int value :int] :int
 * Returns 1 on success, 0 on out-of-range. */
static TuriValue native_safe_array_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_int(0);
    int64_t *arr = (int64_t *)(intptr_t)a[0].as_int;
    int64_t  idx = a[1].as_int;
    int64_t  val = a[2].as_int;
    if (arr && idx >= 0 && idx < 1024) {
        arr[idx] = val;
        return turi_int(1);
    }
    return turi_int(0);
}

/* box [v :int] :ptr -- allocate an int64_t on the heap */
static TuriValue native_safe_box(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t *p = (int64_t *)malloc(sizeof(int64_t));
    if (!p) return turi_nil();
    *p = (n > 0) ? a[0].as_int : 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)p;
    return v;
}

/* unbox [p :ptr] :int -- read int64_t from heap pointer */
static TuriValue native_safe_unbox(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *p = (int64_t *)(intptr_t)a[0].as_int;
    if (!p) return turi_int(0);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = *p;
    return v;
}

void wk_register_safe_natives(TuriEnv *env) {
    turi_env_register_native(env, "array-get",   native_safe_array_get, NULL);
    turi_env_register_native(env, "array-set",   native_safe_array_set, NULL);
    turi_env_register_native(env, "box",         native_safe_box,       NULL);
    turi_env_register_native(env, "unbox",       native_safe_unbox,     NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): comonad.tur native shims.
 *
 * comonad.tur's Identity / Pair cells are malloc'd heap structs whose pointer
 * rides the int64 carrier (the compiled ABI).  The inline-C accessors
 * (__identity_new/_get/_extract/_duplicate, __pair_new/_env/_val/_extract/
 * _duplicate) are re-implemented over the identical layout; the *_extend ops
 * are pure-turi (they call (f wa) then a native constructor) and need no shim.
 *   Identity = { int64_t value; }
 *   Pair     = Tuple2 { int64_t e1 (env); int64_t e2 (val); }
 * ---------------------------------------------------------------------- */
typedef struct { int64_t e1; int64_t e2; } WkTuple2;

static TuriValue wk_int_ptr(void *p) {
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)p; return v;
}

static TuriValue native_identity_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t *p = (int64_t *)malloc(sizeof(int64_t));
    if (!p) return turi_nil();
    *p = (n > 0) ? a[0].as_int : 0;
    return wk_int_ptr(p);
}
static TuriValue native_identity_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *p = (int64_t *)(intptr_t)a[0].as_int;
    return p ? turi_int(*p) : turi_int(0);
}
static TuriValue native_identity_duplicate(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    /* duplicate w = Identity w (wrap the original carrier) */
    int64_t *p = (int64_t *)malloc(sizeof(int64_t));
    if (!p) return turi_nil();
    *p = (n > 0) ? a[0].as_int : 0;
    return wk_int_ptr(p);
}
static TuriValue native_pair_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    WkTuple2 *p = (WkTuple2 *)malloc(sizeof(WkTuple2));
    if (!p) return turi_nil();
    p->e1 = (n > 0) ? a[0].as_int : 0;
    p->e2 = (n > 1) ? a[1].as_int : 0;
    return wk_int_ptr(p);
}
static TuriValue native_pair_env(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    WkTuple2 *p = (WkTuple2 *)(intptr_t)a[0].as_int;
    return p ? turi_int(p->e1) : turi_int(0);
}
static TuriValue native_pair_val(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    WkTuple2 *p = (WkTuple2 *)(intptr_t)a[0].as_int;
    return p ? turi_int(p->e2) : turi_int(0);
}
static TuriValue native_pair_duplicate(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    WkTuple2 *src = (WkTuple2 *)(intptr_t)a[0].as_int;
    if (!src) return turi_nil();
    WkTuple2 *p = (WkTuple2 *)malloc(sizeof(WkTuple2));
    if (!p) return turi_nil();
    p->e1 = src->e1;
    p->e2 = a[0].as_int;  /* inner = the original cell */
    return wk_int_ptr(p);
}

static void wk_register_comonad_natives(TuriEnv *env) {
    turi_env_register_native(env, "__identity_new",       native_identity_new,       NULL);
    turi_env_register_native(env, "__identity_get",       native_identity_get,       NULL);
    turi_env_register_native(env, "__identity_extract",   native_identity_get,       NULL);
    turi_env_register_native(env, "__identity_duplicate", native_identity_duplicate, NULL);
    turi_env_register_native(env, "__pair_new",           native_pair_new,           NULL);
    turi_env_register_native(env, "__pair_env",           native_pair_env,           NULL);
    turi_env_register_native(env, "__pair_val",           native_pair_val,           NULL);
    turi_env_register_native(env, "__pair_extract",       native_pair_val,           NULL);
    turi_env_register_native(env, "__pair_duplicate",     native_pair_duplicate,     NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): mutex.tur native shims.
 *
 * Faithful pthread_mutex_t over the int64 carrier (the compiled ABI lowers
 * Mutex to :ptr<void>).  The interpreter is single-threaded for these fixtures,
 * but a real mutex keeps lock/unlock/try-lock/free semantics honest.
 * ---------------------------------------------------------------------- */
static TuriValue native_mutex_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (!m) return turi_nil();
    pthread_mutex_init(m, NULL);
    return wk_int_ptr(m);
}
static TuriValue native_mutex_lock(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].as_int) pthread_mutex_lock((pthread_mutex_t *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_mutex_unlock(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].as_int) pthread_mutex_unlock((pthread_mutex_t *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_mutex_try_lock(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_bool(false);
    return turi_bool(pthread_mutex_trylock((pthread_mutex_t *)(intptr_t)a[0].as_int) == 0);
}
static TuriValue native_mutex_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].as_int) {
        pthread_mutex_t *m = (pthread_mutex_t *)(intptr_t)a[0].as_int;
        pthread_mutex_destroy(m);
        free(m);
    }
    return turi_nil();
}

static void wk_register_mutex_natives(TuriEnv *env) {
    turi_env_register_native(env, "mutex-new",      native_mutex_new,      NULL);
    turi_env_register_native(env, "mutex-lock",     native_mutex_lock,     NULL);
    turi_env_register_native(env, "mutex-unlock",   native_mutex_unlock,   NULL);
    turi_env_register_native(env, "mutex-try-lock", native_mutex_try_lock, NULL);
    turi_env_register_native(env, "mutex-free",     native_mutex_free,     NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): future.tur FutureCell native shims.
 *
 * Layout-exact replica of future.tur's FutureCell so the refcounted
 * Promise/Future split-free semantics are honest under --interpret:
 * future-handle bumps refcount; future-cell-free / future-free drop a ref and
 * tear down at zero (the historical double-free hazard the fixture guards
 * against).  promise-new / promise-free are pure-turi wrappers over
 * future-cell-new / future-cell-free, so only these four need natives.
 * ---------------------------------------------------------------------- */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  ready;
    int64_t value;
    int64_t exn;
    bool is_set;
    bool is_ok;
    int64_t refcount;
} WkFutureCell;

static TuriValue native_future_cell_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    WkFutureCell *cell = (WkFutureCell *)malloc(sizeof(WkFutureCell));
    if (!cell) return turi_nil();
    pthread_mutex_init(&cell->lock, NULL);
    pthread_cond_init(&cell->ready, NULL);
    cell->value = 0; cell->exn = 0; cell->is_set = false; cell->is_ok = false;
    cell->refcount = 1;
    return wk_int_ptr(cell);
}
static TuriValue native_future_handle(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkFutureCell *fc = (WkFutureCell *)(intptr_t)a[0].as_int;
    __atomic_add_fetch(&fc->refcount, 1, __ATOMIC_ACQ_REL);
    return wk_int_ptr(fc);
}
static TuriValue native_future_cell_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkFutureCell *fc = (WkFutureCell *)(intptr_t)a[0].as_int;
    if (__atomic_sub_fetch(&fc->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        pthread_mutex_destroy(&fc->lock);
        pthread_cond_destroy(&fc->ready);
        free(fc);
    }
    return turi_nil();
}

static TuriValue native_promise_fulfill(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkFutureCell *fc = (WkFutureCell *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&fc->lock);
    if (!fc->is_set) {
        fc->value = (n > 1) ? a[1].as_int : 0;
        fc->is_set = true;
        fc->is_ok = true;
        pthread_cond_broadcast(&fc->ready);
    }
    pthread_mutex_unlock(&fc->lock);
    return turi_nil();
}
static TuriValue native_future_done(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_bool(false);
    WkFutureCell *fc = (WkFutureCell *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&fc->lock);
    bool done = fc->is_set;
    pthread_mutex_unlock(&fc->lock);
    return turi_bool(done);
}

static void wk_register_future_natives(TuriEnv *env) {
    turi_env_register_native(env, "future-cell-new",  native_future_cell_new,  NULL);
    turi_env_register_native(env, "future-handle",    native_future_handle,    NULL);
    turi_env_register_native(env, "future-cell-free", native_future_cell_free, NULL);
    turi_env_register_native(env, "future-free",      native_future_cell_free, NULL);
    turi_env_register_native(env, "promise-fulfill",  native_promise_fulfill,  NULL);
    turi_env_register_native(env, "future-done?",     native_future_done,      NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): chan.tur bounded-channel shims.
 *
 * The interpreter is single-threaded, so a real cond-var-blocking channel
 * would only ever deadlock; the faithful interpreter semantics for the
 * (single-thread) fixtures is a plain bounded ring buffer guarded by a mutex.
 * One WkChan backs both the sync (chan-*) and async (async-chan-*) surfaces;
 * the natives own both the allocation and every access, so they need not match
 * the compiled ChanBlock layout (which also carries select waiters).
 * ---------------------------------------------------------------------- */
typedef struct {
    pthread_mutex_t lock;
    int64_t *buf;
    int64_t head, tail, count, cap;
} WkChan;

static TuriValue native_chan_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t cap = (n > 0) ? a[0].as_int : 0;
    if (cap < 1) cap = 1;
    WkChan *ch = (WkChan *)malloc(sizeof(WkChan));
    if (!ch) return turi_nil();
    ch->buf = (int64_t *)malloc(sizeof(int64_t) * (size_t)cap);
    if (!ch->buf) { free(ch); return turi_nil(); }
    pthread_mutex_init(&ch->lock, NULL);
    ch->head = ch->tail = ch->count = 0; ch->cap = cap;
    return wk_int_ptr(ch);
}
/* push; returns true if there was room */
static bool wk_chan_push(WkChan *ch, int64_t v) {
    bool ok = false;
    pthread_mutex_lock(&ch->lock);
    if (ch->count < ch->cap) {
        ch->buf[ch->tail] = v;
        ch->tail = (ch->tail + 1) % ch->cap;
        ch->count++;
        ok = true;
    }
    pthread_mutex_unlock(&ch->lock);
    return ok;
}
/* pop into *out; returns true if a value was available */
static bool wk_chan_pop(WkChan *ch, int64_t *out) {
    bool ok = false;
    pthread_mutex_lock(&ch->lock);
    if (ch->count > 0) {
        *out = ch->buf[ch->head];
        ch->head = (ch->head + 1) % ch->cap;
        ch->count--;
        ok = true;
    }
    pthread_mutex_unlock(&ch->lock);
    return ok;
}
static TuriValue native_chan_send(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n >= 2 && a[0].as_int) wk_chan_push((WkChan *)(intptr_t)a[0].as_int, a[1].as_int);
    return turi_nil();
}
static TuriValue native_chan_recv(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = 0;
    if (n >= 1 && a[0].as_int) wk_chan_pop((WkChan *)(intptr_t)a[0].as_int, &v);
    return turi_int(v);
}
static TuriValue native_chan_try_send(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2 || !a[0].as_int) return turi_bool(false);
    return turi_bool(wk_chan_push((WkChan *)(intptr_t)a[0].as_int, a[1].as_int));
}
static TuriValue native_chan_try_recv(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = 0;
    if (n >= 1 && a[0].as_int) wk_chan_pop((WkChan *)(intptr_t)a[0].as_int, &v);
    return turi_int(v);
}
static TuriValue native_chan_count(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    WkChan *ch = (WkChan *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&ch->lock);
    int64_t c = ch->count;
    pthread_mutex_unlock(&ch->lock);
    return turi_int(c);
}
static TuriValue native_chan_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n >= 1 && a[0].as_int) {
        WkChan *ch = (WkChan *)(intptr_t)a[0].as_int;
        pthread_mutex_destroy(&ch->lock);
        free(ch->buf);
        free(ch);
    }
    return turi_nil();
}

/* schan.tur synchronous session channels: same ring buffer (SChanBlock matches
 * WkChan's leading fields).  The protocol continuation rides the same pointer,
 * so send and the advance step both return the channel carrier unchanged.
 *
 * Only the two INLINE-C leaves are overridden here. `schan-recv` itself is
 * ordinary Turmeric -- `(pair (schan-recv-value c) (schan-advance-recv c))` --
 * so the tree-walker evaluates it and builds the Pair with the same struct
 * machinery the compiled path uses. Overriding `schan-recv` instead would mean
 * hand-building a Pair value here, i.e. a second copy of that layout, which is
 * exactly what the cell out-parameter existed to avoid. */
static TuriValue native_schan_send(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2 || !a[0].as_int) return turi_int(0);
    wk_chan_push((WkChan *)(intptr_t)a[0].as_int, a[1].as_int);
    return a[0];  /* SChan R continuation */
}
static TuriValue native_schan_recv_value(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    int64_t v = 0;
    wk_chan_pop((WkChan *)(intptr_t)a[0].as_int, &v);
    return turi_int(v);
}
static TuriValue native_schan_advance_recv(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    return a[0];  /* same pointer; only the phantom moves */
}

static void wk_register_chan_natives(TuriEnv *env) {
    turi_env_register_native(env, "chan-new",            native_chan_new,      NULL);
    turi_env_register_native(env, "chan-send",           native_chan_send,     NULL);
    turi_env_register_native(env, "chan-recv",           native_chan_recv,     NULL);
    turi_env_register_native(env, "chan-free",           native_chan_free,     NULL);
    turi_env_register_native(env, "async-chan-new",      native_chan_new,      NULL);
    turi_env_register_native(env, "async-chan-try-send", native_chan_try_send, NULL);
    turi_env_register_native(env, "async-chan-try-recv", native_chan_try_recv, NULL);
    turi_env_register_native(env, "async-chan-count",    native_chan_count,    NULL);
    turi_env_register_native(env, "async-chan-free",     native_chan_free,     NULL);
    /* schan.tur synchronous session channels (SChanBlock == WkChan prefix). */
    turi_env_register_native(env, "schan-new",           native_chan_new,        NULL);
    turi_env_register_native(env, "schan-send",          native_schan_send,      NULL);
    turi_env_register_native(env, "schan-recv-value",    native_schan_recv_value,   NULL);
    turi_env_register_native(env, "schan-advance-recv",  native_schan_advance_recv, NULL);
    turi_env_register_native(env, "schan-close",         native_chan_free,       NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): process.tur + fs.tur OS-handle shims.
 *
 * Faithful syscalls: process/spawn forks+execvp's (ChildHandle = pid), wait
 * reaps it; fs/tmpfile mkstemp's into a { path, fd } pair (TmpFile), with
 * borrow-accessors and a close+free.  argv is a cons list of cstr pointers
 * ({head,tail} cells), matching the compiled inline-C walk.
 * ---------------------------------------------------------------------- */
static TuriValue native_process_spawn(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *path = (n > 0 && a[0].tag == TURI_CSTR) ? a[0].as_cstr : NULL;
    if (!path) return turi_int(-1);
    int64_t argv = (n > 1) ? a[1].as_int : 0;
    int argc = 0;
    for (int64_t t = argv; t; t = ((int64_t *)(intptr_t)t)[1]) argc++;
    char **args = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
    if (!args) return turi_int(-1);
    { int i = 0; for (int64_t t = argv; t; t = ((int64_t *)(intptr_t)t)[1])
        args[i++] = (char *)(intptr_t)((int64_t *)(intptr_t)t)[0]; }
    args[argc] = NULL;
#ifdef _WIN32
    /* _spawnvp(_P_NOWAIT) is fork+execvp in one call: it returns a process
     * HANDLE (not a pid) that _cwait() below reaps.  The handle is opaque to
     * callers either way, so ChildHandle keeps its meaning. */
    intptr_t child = _spawnvp(_P_NOWAIT, path, (const char *const *)args);
    free(args);
    if (child == -1) return turi_int(-1);
    return turi_int((int64_t)child);
#else
    pid_t pid = fork();
    if (pid < 0) { free(args); return turi_int(-1); }
    if (pid == 0) { execvp(path, args); _exit(127); }
    free(args);
    return turi_int((int64_t)pid);
#endif
}
static TuriValue native_process_wait(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(-1);
#if defined(__EMSCRIPTEN__)
    /* No process reaping in the browser; process/spawn already returns -1. */
    return turi_int(-1);
#elif defined(_WIN32)
    /* _cwait yields the child's exit code directly -- there is no POSIX
     * wait-status encoding to unpack, so no WIFEXITED/WEXITSTATUS here. */
    int status = 0;
    if (_cwait(&status, (intptr_t)a[0].as_int, 0) == -1) return turi_int(-1);
    return turi_int((int64_t)status);
#else
    int status = 0;
    if (waitpid((pid_t)a[0].as_int, &status, 0) < 0) return turi_int(-1);
    if (WIFEXITED(status)) return turi_int((int64_t)WEXITSTATUS(status));
    return turi_int(-1);
#endif
}
static TuriValue native_fs_tmpfile(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    char *tmpl = strdup("/tmp/tur_XXXXXX");
    if (!tmpl) return turi_int(0);
    int fd = mkstemp(tmpl);
    if (fd < 0) { free(tmpl); return turi_int(0); }
    int64_t *pair = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!pair) { close(fd); free(tmpl); return turi_int(0); }
    pair[0] = (int64_t)(intptr_t)tmpl;
    pair[1] = (int64_t)fd;
    return wk_int_ptr(pair);
}
static TuriValue native_fs_tmpfile_path(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    return turi_cstr((const char *)(intptr_t)((int64_t *)(intptr_t)a[0].as_int)[0]);
}
static TuriValue native_fs_tmpfile_fd(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[1]);
}
static TuriValue native_fs_tmpfile_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    int64_t *pair = (int64_t *)(intptr_t)a[0].as_int;
    close((int)pair[1]);
    free((void *)(intptr_t)pair[0]);
    free(pair);
    return turi_nil();
}

/* -------------------------------------------------------------------------
 * R3 (turi-interpret-flip-residual-plan): backtrack.tur cons-stream monad.
 *
 * A Backtrack value is a linked list of results -- malloc'd { int64 value;
 * int64 next; } cells (next=0 is nil), the exact compiled layout.  All the
 * typeclass instances (Functor/Applicative/Monad/Alternative) and the *-raw
 * workers are pure-turi wrappers over these inline-C primitives, so shimming
 * the primitives makes the whole surface interpretable.  mbind/bt-apply-fat
 * invoke the continuation via turi_call (the interpreter hands it a closure
 * where the compiled path used a fat-closure thunk).
 * ---------------------------------------------------------------------- */
typedef struct { int64_t value; int64_t next; } WkBtCell;

static int64_t wk_bt_cell(int64_t value, int64_t next) {
    WkBtCell *c = (WkBtCell *)malloc(sizeof(WkBtCell));
    if (!c) return 0;
    c->value = value; c->next = next;
    return (int64_t)(intptr_t)c;
}
static TuriValue native_bt_nil(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud; return turi_int(0);
}
static TuriValue native_bt_cons(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = (n > 0) ? a[0].as_int : 0;
    int64_t nx = (n > 1) ? a[1].as_int : 0;
    return turi_int(wk_bt_cell(v, nx));
}
static TuriValue native_bt_mreturn(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    return turi_int(wk_bt_cell((n > 0) ? a[0].as_int : 0, 0));
}
/* mplus: copy xs's spine, then point its tail at ys (ys shared). */
static TuriValue native_bt_mplus(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t xs = (n > 0) ? a[0].as_int : 0;
    int64_t ys = (n > 1) ? a[1].as_int : 0;
    WkBtCell *xc = (WkBtCell *)(intptr_t)xs;
    if (!xc) return turi_int(ys);
    int64_t head = 0; WkBtCell *tail = NULL;
    while (xc) {
        int64_t nc = wk_bt_cell(xc->value, 0);
        if (tail) tail->next = nc; else head = nc;
        tail = (WkBtCell *)(intptr_t)nc;
        xc = (WkBtCell *)(intptr_t)xc->next;
    }
    if (tail) tail->next = ys;
    return turi_int(head);
}
/* mbind: concatMap -- for each value, turi_call fn to get a sub-stream, flatten
 * in order (build reversed, then reverse, matching the compiled body). */
static TuriValue native_bt_mbind(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    WkBtCell *cell = (WkBtCell *)(intptr_t)a[0].as_int;
    int64_t rev = 0;
    while (cell) {
        TuriValue arg = turi_int(cell->value);
        TuriValue sub = turi_call(env, a[1], &arg, 1);
        if (turi_is_error(sub) || env->throwing) return sub;  /* propagate callback error */
        WkBtCell *sc = (WkBtCell *)(intptr_t)sub.as_int;
        while (sc) {
            rev = wk_bt_cell(sc->value, rev);
            sc = (WkBtCell *)(intptr_t)sc->next;
        }
        cell = (WkBtCell *)(intptr_t)cell->next;
    }
    int64_t result = 0;
    WkBtCell *r = (WkBtCell *)(intptr_t)rev;
    while (r) {
        WkBtCell *tmp = (WkBtCell *)(intptr_t)r->next;
        r->next = result;
        result = (int64_t)(intptr_t)r;
        r = tmp;
    }
    return turi_int(result);
}
static TuriValue native_bt_length(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t cnt = 0;
    WkBtCell *c = (n > 0) ? (WkBtCell *)(intptr_t)a[0].as_int : NULL;
    while (c) { cnt++; c = (WkBtCell *)(intptr_t)c->next; }
    return turi_int(cnt);
}
static TuriValue native_bt_print(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    WkBtCell *c = (n > 0) ? (WkBtCell *)(intptr_t)a[0].as_int : NULL;
    while (c) { printf("%lld\n", (long long)c->value); c = (WkBtCell *)(intptr_t)c->next; }
    return turi_nil();
}
static TuriValue native_bt_apply_fat(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    return turi_call(env, a[0], &a[1], 1);
}

/* R6 (turi-interpret-flip-residual-plan): tuple.tur's Eq[Tuple2] instance is
 * pure-turi but bottoms out in `tuple2-eq-carrier?`, an inline-C body that casts
 * each Tuple2 to a { e1, e2 } struct and calls two element comparators through C
 * function pointers.  Re-implement it as a native: read both fields (a make-
 * struct Tuple2 is a TuriStruct; a carrier int points at an int64[2] {e1,e2}),
 * and invoke the comparators (turi closures) via turi_call. */
static int64_t wk_tuple2_field(TuriValue t, int idx) {
    bool found = false;
    TuriValue f = turi_struct_field(t, (uint32_t)idx, &found);
    if (found) return (f.tag == TURI_BOOL) ? (f.as_bool ? 1 : 0) : f.as_int;
    int64_t *p = (int64_t *)(intptr_t)t.as_int;
    return p ? p[idx] : 0;
}
static TuriValue native_tuple2_eq_carrier(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return turi_bool(false);
    int64_t a1 = wk_tuple2_field(a[0], 0), a2 = wk_tuple2_field(a[0], 1);
    int64_t b1 = wk_tuple2_field(a[1], 0), b2 = wk_tuple2_field(a[1], 1);
    TuriValue c1args[2] = { turi_int(a1), turi_int(b1) };
    TuriValue r1 = turi_call(env, a[2], c1args, 2);
    if (turi_is_error(r1) || env->throwing) return r1;  /* propagate callback error */
    bool e1 = (r1.tag == TURI_BOOL) ? r1.as_bool : (r1.as_int != 0);
    if (!e1) return turi_bool(false);
    TuriValue c2args[2] = { turi_int(a2), turi_int(b2) };
    TuriValue r2 = turi_call(env, a[3], c2args, 2);
    if (turi_is_error(r2) || env->throwing) return r2;  /* propagate callback error */
    bool e2 = (r2.tag == TURI_BOOL) ? r2.as_bool : (r2.as_int != 0);
    return turi_bool(e2);
}

static void wk_register_backtrack_natives(TuriEnv *env) {
    turi_env_register_native(env, "tuple2-eq-carrier?", native_tuple2_eq_carrier, NULL);
    turi_env_register_native(env, "bt-nil",        native_bt_nil,       NULL);
    turi_env_register_native(env, "bt-cons",       native_bt_cons,      NULL);
    turi_env_register_native(env, "mzero",         native_bt_nil,       NULL);
    turi_env_register_native(env, "mreturn",       native_bt_mreturn,   NULL);
    turi_env_register_native(env, "mplus",         native_bt_mplus,     NULL);
    turi_env_register_native(env, "mbind",         native_bt_mbind,     NULL);
    turi_env_register_native(env, "bt-length",     native_bt_length,    NULL);
    turi_env_register_native(env, "bt-print",      native_bt_print,     NULL);
    turi_env_register_native(env, "bt-apply-fat",  native_bt_apply_fat, NULL);
}

static void wk_register_proc_fs_natives(TuriEnv *env) {
    turi_env_register_native(env, "process/spawn",    native_process_spawn,     NULL);
    turi_env_register_native(env, "process/wait",     native_process_wait,      NULL);
    turi_env_register_native(env, "fs/tmpfile",       native_fs_tmpfile,        NULL);
    turi_env_register_native(env, "fs/tmpfile-path",  native_fs_tmpfile_path,   NULL);
    turi_env_register_native(env, "fs/tmpfile-fd",    native_fs_tmpfile_fd,     NULL);
    turi_env_register_native(env, "fs/tmpfile-free",  native_fs_tmpfile_free,   NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): serial.tur Serializable instances.
 *
 * serialize packs a length-prefixed little-endian byte buffer (int64* header
 * word 0 = data length, data starts at word 1 -- the same layout as Bytes);
 * deserialize reads it back.  Registered under the elaborator instance-binding
 * names so (.serialize x) dispatch and the direct __inst_* calls both resolve.
 * ---------------------------------------------------------------------- */
static TuriValue native_serialize_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = (n > 0) ? a[0].as_int : 0;
    int64_t *buf = (int64_t *)malloc(sizeof(int64_t) + 8);
    if (!buf) return turi_nil();
    buf[0] = 8;
    uint8_t *p = (uint8_t *)(buf + 1);
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
    return wk_int_ptr(buf);
}
static TuriValue native_deserialize_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    int64_t *b = (int64_t *)(intptr_t)a[0].as_int;
    if (b[0] < 8) return turi_int(0);
    uint8_t *p = (uint8_t *)(b + 1);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
    return turi_int((int64_t)v);
}
static TuriValue native_serialize_bool(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = 0;
    if (n > 0) v = (a[0].tag == TURI_BOOL) ? (a[0].as_bool ? 1 : 0) : (a[0].as_int != 0);
    int64_t *buf = (int64_t *)malloc(sizeof(int64_t) + 1);
    if (!buf) return turi_nil();
    buf[0] = 1;
    *(uint8_t *)(buf + 1) = (uint8_t)v;
    return wk_int_ptr(buf);
}
static TuriValue native_deserialize_bool(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    int64_t *b = (int64_t *)(intptr_t)a[0].as_int;
    if (b[0] < 1) return turi_int(0);
    return turi_int(*(uint8_t *)(b + 1) ? 1 : 0);
}

static void wk_register_serial_natives(TuriEnv *env) {
    turi_env_register_native(env, "__inst_Serializable_serialize_int",     native_serialize_int,     NULL);
    turi_env_register_native(env, "__inst_Serializable_deserialize_int",   native_deserialize_int,   NULL);
    turi_env_register_native(env, "__inst_Serializable_serialize_bool",    native_serialize_bool,    NULL);
    turi_env_register_native(env, "__inst_Serializable_deserialize_bool",  native_deserialize_bool,  NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): serial.tur Bytes native shims.
 *
 * Bytes is a malloc'd int64_t* whose word 0 holds the length and whose data
 * starts at word 1 (the compiled ABI).  bytes-alloc/len/data/free re-implement
 * that layout exactly.
 * ---------------------------------------------------------------------- */
static TuriValue native_bytes_alloc(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t len = (n > 0) ? a[0].as_int : 0;
    if (len < 0) len = 0;
    int64_t *buf = (int64_t *)malloc(sizeof(int64_t) + (size_t)len);
    if (!buf) return turi_nil();
    buf[0] = len;
    memset(buf + 1, 0, (size_t)len);
    return wk_int_ptr(buf);
}
static TuriValue native_bytes_len(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[0]);
}
static TuriValue native_bytes_data(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    return wk_int_ptr((int64_t *)(intptr_t)a[0].as_int + 1);
}
static TuriValue native_bytes_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].as_int) free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}

static void wk_register_bytes_natives(TuriEnv *env) {
    turi_env_register_native(env, "bytes-alloc", native_bytes_alloc, NULL);
    turi_env_register_native(env, "bytes-len",   native_bytes_len,   NULL);
    turi_env_register_native(env, "bytes-data",  native_bytes_data,  NULL);
    turi_env_register_native(env, "bytes-free",  native_bytes_free,  NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): taskgroup.tur TaskGroupBlock shims.
 *
 * Replica of taskgroup.tur's TaskGroupBlock.  We include cancel_reason in the
 * allocation (the canonical documented layout) so cancel-with-reason is safe;
 * taskgroup.tur's own `task-group-new` inline-C declares and allocates the same
 * trailing field, so the two layouts agree -- keep them in step when either
 * side gains a field.
 *
 * The cancel native does NOT touch the per-fiber thread-local cancelled flag
 * (tur_fiber_set_cancelled) that the inline-C body sets: under --interpret no
 * fibers are running, and the fixtures observe only the group's own flag.
 * ---------------------------------------------------------------------- */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t done_cond;
    int64_t task_count;
    int64_t completed_count;
    bool cancelled;
    bool done;
    int64_t cancel_reason;
} WkTaskGroup;

static TuriValue native_task_group_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    WkTaskGroup *g = (WkTaskGroup *)malloc(sizeof(WkTaskGroup));
    if (!g) return turi_nil();
    pthread_mutex_init(&g->lock, NULL);
    pthread_cond_init(&g->done_cond, NULL);
    g->task_count = 0; g->completed_count = 0;
    g->cancelled = false; g->done = false; g->cancel_reason = 0;
    return wk_int_ptr(g);
}
static TuriValue native_task_group_cancel(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkTaskGroup *g = (WkTaskGroup *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&g->lock);
    g->cancelled = true; g->done = true; g->cancel_reason = 0;
    pthread_cond_broadcast(&g->done_cond);
    pthread_mutex_unlock(&g->lock);
    return turi_nil();
}
static TuriValue native_task_group_wait(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkTaskGroup *g = (WkTaskGroup *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&g->lock);
    while (!g->done) pthread_cond_wait(&g->done_cond, &g->lock);
    pthread_mutex_unlock(&g->lock);
    return turi_nil();
}
static TuriValue native_task_group_cancelled(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_bool(false);
    WkTaskGroup *g = (WkTaskGroup *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&g->lock);
    bool c = g->cancelled;
    pthread_mutex_unlock(&g->lock);
    return turi_bool(c);
}
static TuriValue native_task_group_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkTaskGroup *g = (WkTaskGroup *)(intptr_t)a[0].as_int;
    pthread_mutex_destroy(&g->lock);
    pthread_cond_destroy(&g->done_cond);
    free(g);
    return turi_nil();
}

static void wk_register_taskgroup_natives(TuriEnv *env) {
    turi_env_register_native(env, "task-group-new",        native_task_group_new,       NULL);
    turi_env_register_native(env, "task-group-cancel",     native_task_group_cancel,    NULL);
    turi_env_register_native(env, "task-group-wait",       native_task_group_wait,      NULL);
    turi_env_register_native(env, "task-group-cancelled?", native_task_group_cancelled, NULL);
    turi_env_register_native(env, "task-group-free",       native_task_group_free,      NULL);
}

/* -------------------------------------------------------------------------
 * Native overrides for typeclass instance methods with inline-C bodies.
 * These are registered under the elaborator-generated C binding names
 * (e.g. __inst_Show_show_int) so that eval_apply can find them when a
 * function has an EX_INLINE_C body.
 * ---------------------------------------------------------------------- */

/* Show [int].show: format int64 as decimal string */
static TuriValue native_show_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = (n > 0) ? a[0].as_int : 0;
    char *buf = (char *)malloc(32);
    if (!buf) return turi_nil();
    snprintf(buf, 32, "%lld", (long long)v);
    /* Note: this leaks 'buf'. Acceptable for interpreter mode. */
    return turi_cstr(buf);
}

/* show-float: standalone show function for floats (used in show-float fixture) */
static TuriValue native_show_float_fn(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double v = (n > 0 && a[0].tag == TURI_FLOAT) ? a[0].as_float : 0.0;
    char *buf = (char *)malloc(64);
    if (!buf) return turi_nil();
    snprintf(buf, 64, "%g", v);
    return turi_cstr(buf);
}

/* ---- Show instance-method fallbacks: return an OWNED String -----------------
 * show-owned-result-plan stage 4/5: the stdlib `Show` renders to an owned
 * `String`, and its numeric/float instance bodies are inline-C the tree-walker
 * cannot execute.  These fallbacks build a real String (via tur_string_from_*)
 * and box the handle as turi_int -- the same representation string_native.c
 * uses -- so `(show x)` under `--interpret` returns a releasable String, not a
 * bare cstr (which string/to-cstr / string/release would then misread). */
static TuriValue show_str_of_cstr(const char *s) {
    return turi_int((int64_t)(intptr_t)tur_string_from_cstr(s));
}
static TuriValue native_show_int_str(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_string_from_int((n > 0) ? a[0].as_int : 0));
}
static TuriValue native_show_uint_str(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    char buf[24];
    snprintf(buf, sizeof buf, "%llu",
             (unsigned long long)((n > 0) ? (uint64_t)a[0].as_int : 0));
    return show_str_of_cstr(buf);
}
static TuriValue native_show_float_str(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double v = (n > 0 && a[0].tag == TURI_FLOAT) ? a[0].as_float : 0.0;
    char buf[32];
    snprintf(buf, sizeof buf, "%g", v);
    return show_str_of_cstr(buf);
}
static TuriValue native_show_bool_str(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    bool v = (n > 0 && a[0].tag == TURI_BOOL) ? a[0].as_bool : false;
    return show_str_of_cstr(v ? "true" : "false");
}
static TuriValue native_show_cstr_str(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    return show_str_of_cstr((n > 0 && a[0].tag == TURI_CSTR) ? json_arg_cstr(a[0]) : "");
}

/* show-string-fputs: write a cstr to stdout with no trailing newline.
 *
 * stdlib/typeclass-show.tur spells this as an inline-C `fputs`, which the
 * tree-walker cannot execute -- so `print-show` (the only caller) aborted the
 * whole program under `--interpret` even though `show` / `show-line` interpret
 * fine.  Overriding it natively keeps `print-show` on the interpreted path. */
static TuriValue native_show_string_fputs(TuriEnv *env, TuriValue *a,
                                          uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *s = (n > 0) ? json_arg_cstr(a[0]) : NULL;
    if (s) fputs(s, stdout);
    return turi_nil();
}

void wk_register_typeclass_natives(TuriEnv *env) {
    /* Show typeclass instances -- return owned String (stage 4/5).  Signed
     * fixed-width types carry as int64 in the interpreter, so they share the
     * int fallback; unsigned share the %llu fallback. */
    turi_env_register_native(env, "__inst_Show_show_int",     native_show_int_str,   NULL);
    turi_env_register_native(env, "__inst_Show_show_int8",    native_show_int_str,   NULL);
    turi_env_register_native(env, "__inst_Show_show_int16",   native_show_int_str,   NULL);
    turi_env_register_native(env, "__inst_Show_show_int32",   native_show_int_str,   NULL);
    turi_env_register_native(env, "__inst_Show_show_uint8",   native_show_uint_str,  NULL);
    turi_env_register_native(env, "__inst_Show_show_uint16",  native_show_uint_str,  NULL);
    turi_env_register_native(env, "__inst_Show_show_uint32",  native_show_uint_str,  NULL);
    turi_env_register_native(env, "__inst_Show_show_uint64",  native_show_uint_str,  NULL);
    turi_env_register_native(env, "__inst_Show_show_float",   native_show_float_str, NULL);
    turi_env_register_native(env, "__inst_Show_show_float32", native_show_float_str, NULL);
    /* NOTE: do NOT register the carrier-fallback `__inst_Show_show_T` here.
     * The `_T` suffix is the ABSTRACT/carrier mangling; pre-binding it would
     * silently HIJACK any user Show instance over a carrier-typed receiver
     * (opaque handle, rc<T>, ...) and mis-render it.  Leaving it unbound lets
     * the user's own instance method resolve. */
    turi_env_register_native(env, "__inst_Show_show_bool",  native_show_bool_str,  NULL);
    turi_env_register_native(env, "__inst_Show_show_cstr",  native_show_cstr_str,  NULL);
    /* Standalone cstr show helpers (NOT the Show class): used by fixtures that
     * call bare show-int/show-float and expect a cstr. */
    turi_env_register_native(env, "show-float", native_show_float_fn, NULL);
    turi_env_register_native(env, "show-int",   native_show_int,      NULL);
    /* print-show's output helper -- inline-C in the stdlib, native here. */
    turi_env_register_native(env, "show-string-fputs",
                             native_show_string_fputs, NULL);
}

/* Native implementation of tur-contract-check (bool * cstr -> void).
 * Panics (exit 1) if the condition is false; otherwise returns nil. */
TuriValue native_contract_check(TuriEnv *env, TuriValue *args,
                                        uint32_t n, void *ud) {
    (void)env; (void)ud;
    bool cond = true;
    if (n >= 1) {
        TuriValue a = args[0];
        if (a.tag == TURI_BOOL)      cond = a.as_bool;
        else if (a.tag == TURI_INT)  cond = (a.as_int != 0);
        else if (a.tag == TURI_NIL)  cond = false;
    }
    if (!cond) {
        const char *msg = (n >= 2 && args[1].tag == TURI_CSTR && args[1].as_cstr)
                          ? args[1].as_cstr : "Assertion failed";
        fprintf(stderr, "panic at\n%s\n", msg);
        fflush(stderr);
        exit(1);
    }
    return turi_nil();
}

/* Native implementation of tur-contract-check-inv (obj pred msg -> void).
 * Calls pred(obj); panics if it returns false. */
TuriValue native_contract_check_inv(TuriEnv *env, TuriValue *args,
                                            uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_nil();
    TuriValue obj  = args[0];
    TuriValue pred = args[1];
    const char *msg = (args[2].tag == TURI_CSTR && args[2].as_cstr)
                      ? args[2].as_cstr : "Invariant failed";
    if (pred.tag != TURI_CLOSURE || !pred.as_closure) {
        fprintf(stderr, "panic at\n%s (bad predicate)\n", msg);
        fflush(stderr);
        exit(1);
    }
    TuriValue result = turi_call(env, pred, &obj, 1);
    if (turi_is_error(result) || env->throwing) return result;  /* propagate callback error */
    bool ok = true;
    if (result.tag == TURI_BOOL)     ok = result.as_bool;
    else if (result.tag == TURI_INT) ok = (result.as_int != 0);
    else if (result.tag == TURI_NIL) ok = false;
    if (!ok) {
        fprintf(stderr, "panic at\n%s\n", msg);
        fflush(stderr);
        exit(1);
    }
    return turi_nil();
}

/* Native contract-enabled? -- always returns true in worker mode. */
TuriValue native_contract_enabled(TuriEnv *env, TuriValue *args,
                                          uint32_t n, void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_bool(true);
}

/* -------------------------------------------------------------------------
 * Public entry point: register the full interpreter native override set.
 *
 * This is the exact sequence src/main.c's cmd_eval ran inline; relocated here
 * so the WASM REPL (src/web/wasm_glue.c) and `tur repl` (src/turi/repl.c) call
 * the same block and cannot drift.  Call AFTER turi_env_preload_*.
 * ---------------------------------------------------------------------- */
/* ============================================================================
 * SYNTAX natives -- first-class syntax objects (TURI_SYNTAX wrapping a
 * compiler Form*).  Stage 1 of docs/upcoming/macro-system-direction-plan.md:
 * the value plumbing that macro-time evaluation (Stage 2's defmacro*) will
 * run on, landed first as REPL/interpreter surface so it is exercisable on
 * its own.  Forms constructed here are allocated from env->sym_arena
 * (env-lifetime; never scratch) and symbols intern into env->st.
 * ==========================================================================*/

static const char *sx_tag_name(TuriTag t) {
    switch (t) {
        case TURI_NIL:    return "nil";
        case TURI_BOOL:   return "bool";
        case TURI_INT:    return "int";
        case TURI_FLOAT:  return "float";
        case TURI_CSTR:   return "cstr";
        case TURI_SYNTAX: return "syntax";
        default:          return "value";
    }
}

/* Arg guard: the wrapped Form* when args[i] is a syntax object, else NULL. */
static Form *sx_arg(TuriValue *a, uint32_t n, uint32_t i) {
    if (i >= n || a[i].tag != TURI_SYNTAX) return NULL;
    return a[i].as_syntax;
}

static bool sx_is_seq(const Form *f) {
    return f && (f->tag == F_LIST || f->tag == F_VEC);
}

static TuriValue native_read_string(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].tag != TURI_CSTR || !a[0].as_cstr)
        return turi_errorf("read-string: expected a string, got %s",
                           n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    /* Copy the source into the env-lifetime arena so any Form slices that
     * borrow from it stay valid after the caller's string dies. */
    size_t len = strlen(a[0].as_cstr);
    char *src = (char *)arena_alloc(&e->sym_arena, len + 1);
    memcpy(src, a[0].as_cstr, len + 1);

    /* The reader reports through the diagnostic file registry, which the
     * enclosing eval (or, when called from a defmacro* body, the enclosing
     * COMPILE) owns right now -- snapshot the whole registry and the
     * had-error flag, parse under a temporary file entry, put both back.
     * diag_files_restore deliberately skips id 0, so re-register the saved
     * entries directly. */
    bool saved_had = diag_had_error();
    const SourceFile *saved_files[64];
    size_t n_saved = diag_files_save(saved_files, 64);
    diag_reset();
    SourceFile sfile = {0};
    sfile.path        = "<read-string>";
    sfile.src         = src;
    sfile.len         = len;
    sfile.file_id     = 0;
    sfile.reader_type = e->reader_type;
    diag_register_file(&sfile);

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&e->sym_arena, &e->st, &sfile,
                                          e->reader_macros, &nforms);
    bool bad = (!forms || nforms == 0 || diag_had_error());
    diag_reset();
    for (size_t i = 0; i < n_saved; i++)
        if (saved_files[i]) diag_register_file(saved_files[i]);
    if (saved_had) diag_force_had_error();
    if (bad)
        return turi_errorf("read-string: could not parse \"%s\"", src);
    /* First form only (Clojure read-string semantics). */
    return turi_syntax_val(forms[0]);
}

static TuriValue native_syntax_tag(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Form *f = sx_arg(a, n, 0);
    if (!f) return turi_errorf("syntax-tag: expected a syntax object, got %s",
                               n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    return turi_cstr(form_tag_name(f->tag));
}

static TuriValue native_syntax_len(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Form *f = sx_arg(a, n, 0);
    if (!f) return turi_errorf("syntax-len: expected a syntax object, got %s",
                               n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    return turi_int(sx_is_seq(f) ? (int64_t)f->as.list.len : 0);
}

static TuriValue native_syntax_first(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Form *f = sx_arg(a, n, 0);
    if (!f) return turi_errorf("syntax-first: expected a syntax object, got %s",
                               n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    if (!sx_is_seq(f) || f->as.list.len == 0) return turi_nil();
    return turi_syntax_val(f->as.list.items[0]);
}

static TuriValue native_syntax_rest(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    Form *f = sx_arg(a, n, 0);
    if (!f) return turi_errorf("syntax-rest: expected a syntax object, got %s",
                               n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    if (!sx_is_seq(f) || f->as.list.len <= 1)
        return turi_syntax_val(form_list(&e->sym_arena, f ? f->span : SPAN_UNKNOWN, NULL, 0));
    uint32_t len = f->as.list.len - 1;
    Form **items = (Form **)arena_alloc(&e->sym_arena, len * sizeof(Form *));
    for (uint32_t i = 0; i < len; i++) items[i] = f->as.list.items[i + 1];
    return turi_syntax_val(form_list(&e->sym_arena, f->span, items, len));
}

static TuriValue native_syntax_nth(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Form *f = sx_arg(a, n, 0);
    if (!f || n < 2 || a[1].tag != TURI_INT)
        return turi_errorf("syntax-nth: expected (syntax-nth stx idx)");
    int64_t idx = a[1].as_int;
    if (!sx_is_seq(f) || idx < 0 || (uint64_t)idx >= f->as.list.len) return turi_nil();
    return turi_syntax_val(f->as.list.items[idx]);
}

/* Tag predicates. ud carries the FormTag to test (int-encoded). */
static TuriValue native_syntax_tag_p(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e;
    Form *f = sx_arg(a, n, 0);
    if (!f) return turi_bool(false);
    return turi_bool(f->tag == (FormTag)(intptr_t)ud);
}

static TuriValue native_syntax_to_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Form *f = sx_arg(a, n, 0);
    if (!f) return turi_errorf("syntax->int: expected a syntax object, got %s",
                               n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    if (f->tag == F_INT)  return turi_int(f->as.i);
    if (f->tag == F_BOOL) return turi_int(f->as.b ? 1 : 0);
    return turi_errorf("syntax->int: form is %s, not an int literal",
                       form_tag_name(f->tag));
}

static TuriValue native_syntax_to_float(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Form *f = sx_arg(a, n, 0);
    if (!f) return turi_errorf("syntax->float: expected a syntax object, got %s",
                               n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    if (f->tag == F_FLOAT) return turi_float(f->as.f);
    if (f->tag == F_INT)   return turi_float((double)f->as.i);
    return turi_errorf("syntax->float: form is %s, not a float literal",
                       form_tag_name(f->tag));
}

static TuriValue native_syntax_to_str(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    Form *f = sx_arg(a, n, 0);
    if (!f) return turi_errorf("syntax->str: expected a syntax object, got %s",
                               n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    if (f->tag != F_STR)
        return turi_errorf("syntax->str: form is %s, not a string literal",
                           form_tag_name(f->tag));
    /* F_STR slices are not NUL-terminated; copy into the value pool. */
    char *s = (char *)turi_val_alloc(e, f->as.s.len + 1);
    memcpy(s, f->as.s.p, f->as.s.len);
    s[f->as.s.len] = '\0';
    return turi_cstr(s);
}

static TuriValue native_syntax_sym_name(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Form *f = sx_arg(a, n, 0);
    if (!f) return turi_errorf("syntax-sym-name: expected a syntax object, got %s",
                               n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    if (f->tag != F_SYM && f->tag != F_KEYWORD)
        return turi_errorf("syntax-sym-name: form is %s, not a symbol/keyword",
                           form_tag_name(f->tag));
    return turi_cstr(f->as.sym->name);
}

static TuriValue native_syntax_to_string(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    Form *f = sx_arg(a, n, 0);
    if (!f) return turi_errorf("syntax->string: expected a syntax object, got %s",
                               n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    Buf b;
    buf_init(&b);
    form_print(&b, f);
    char *s = (char *)turi_val_alloc(e, b.len + 1);
    memcpy(s, b.data, b.len);
    s[b.len] = '\0';
    buf_free(&b);
    return turi_cstr(s);
}

static TuriValue native_int_to_syntax(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].tag != TURI_INT)
        return turi_errorf("int->syntax: expected an int, got %s",
                           n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    return turi_syntax_val(form_int(&e->sym_arena, SPAN_UNKNOWN, a[0].as_int));
}

static TuriValue native_float_to_syntax(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].tag != TURI_FLOAT)
        return turi_errorf("float->syntax: expected a float, got %s",
                           n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    return turi_syntax_val(form_float(&e->sym_arena, SPAN_UNKNOWN, a[0].as_float));
}

static TuriValue native_bool_to_syntax(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].tag != TURI_BOOL)
        return turi_errorf("bool->syntax: expected a bool, got %s",
                           n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    return turi_syntax_val(form_bool(&e->sym_arena, SPAN_UNKNOWN, a[0].as_bool));
}

static TuriValue native_str_to_syntax(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].tag != TURI_CSTR || !a[0].as_cstr)
        return turi_errorf("str->syntax: expected a string, got %s",
                           n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    size_t len = strlen(a[0].as_cstr);
    char *copy = (char *)arena_alloc(&e->sym_arena, len + 1);
    memcpy(copy, a[0].as_cstr, len + 1);
    return turi_syntax_val(form_str(&e->sym_arena, SPAN_UNKNOWN, copy, (uint32_t)len));
}

static TuriValue native_sym_to_syntax(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1)
        return turi_errorf("sym->syntax: expected a name (string or :Sym)");
    /* Dual arg shape like str->sym: a real cstr, or a :Sym int carrier. */
    const Symbol *sym = NULL;
    if (a[0].tag == TURI_CSTR && a[0].as_cstr) {
        StrSlice sl = { a[0].as_cstr, (uint32_t)strlen(a[0].as_cstr) };
        sym = symtab_intern(&e->st, sl);
    } else if (a[0].tag == TURI_INT && a[0].as_int) {
        sym = (const Symbol *)(intptr_t)a[0].as_int;
    }
    if (!sym)
        return turi_errorf("sym->syntax: expected a name (string or :Sym), got %s",
                           sx_tag_name(a[0].tag));
    return turi_syntax_val(form_sym(&e->sym_arena, SPAN_UNKNOWN, sym));
}

/* Shared body for syntax-list / syntax-vec.  ud: 0 = list, 1 = vec.
 * Every element must itself be a syntax object; the result inherits the
 * first element's span so diagnostics against constructed forms still point
 * somewhere real. */
static TuriValue native_syntax_seq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    bool vec = ud != NULL;
    Form **items = (n == 0) ? NULL
                            : (Form **)arena_alloc(&e->sym_arena, n * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) {
        if (a[i].tag != TURI_SYNTAX)
            return turi_errorf("%s: arg %u is %s, not a syntax object",
                               vec ? "syntax-vec" : "syntax-list",
                               i, sx_tag_name(a[i].tag));
        items[i] = a[i].as_syntax;
    }
    Span span = (n > 0 && items[0]) ? items[0]->span : SPAN_UNKNOWN;
    return turi_syntax_val(vec ? form_vec(&e->sym_arena, span, items, n)
                               : form_list(&e->sym_arena, span, items, n));
}

static TuriValue native_syntax_cons(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    Form *hd = sx_arg(a, n, 0);
    Form *tl = sx_arg(a, n, 1);
    if (!hd || !tl)
        return turi_errorf("syntax-cons: expected (syntax-cons stx stx-list)");
    /* Mirror the CT evaluator's cons: list tail prepends; nil tail makes a
     * one-element list; any other tail makes a two-element list. */
    uint32_t tail_len = 0;
    if (tl->tag == F_LIST) tail_len = tl->as.list.len;
    else if (tl->tag != F_NIL) tail_len = 1;
    Form **items = (Form **)arena_alloc(&e->sym_arena, (tail_len + 1) * sizeof(Form *));
    items[0] = hd;
    if (tl->tag == F_LIST) {
        for (uint32_t i = 0; i < tail_len; i++) items[i + 1] = tl->as.list.items[i];
    } else if (tl->tag != F_NIL) {
        items[1] = tl;
    }
    return turi_syntax_val(form_list(&e->sym_arena, hd->span, items, tail_len + 1));
}

static TuriValue native_kw_to_syntax(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1 || a[0].tag != TURI_CSTR || !a[0].as_cstr)
        return turi_errorf("kw->syntax: expected a name string (without the colon), got %s",
                           n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    StrSlice sl = { a[0].as_cstr, (uint32_t)strlen(a[0].as_cstr) };
    const Symbol *sym = symtab_intern(&e->st, sl);
    return turi_syntax_val(form_keyword(&e->sym_arena, SPAN_UNKNOWN, sym));
}

static TuriValue native_nil_to_syntax(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)a; (void)n; (void)ud;
    return turi_syntax_val(form_nil(&e->sym_arena, SPAN_UNKNOWN));
}

static TuriValue native_syntax_type_ann(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    Form *inner = sx_arg(a, n, 0);
    if (!inner)
        return turi_errorf("syntax-type-ann: expected a syntax object, got %s",
                           n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    return turi_syntax_val(form_type_ann(&e->sym_arena, inner->span, inner));
}

static TuriValue native_syntax_quote(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    Form *inner = sx_arg(a, n, 0);
    if (!inner)
        return turi_errorf("syntax-quote: expected a syntax object, got %s",
                           n >= 1 ? sx_tag_name(a[0].tag) : "no argument");
    return turi_syntax_val(form_quote(&e->sym_arena, inner->span, inner));
}

/* syntax-append: concatenate list-shaped syntax objects into one list form.
 * The lowering target for `~@` splices inside a defmacro* quasiquote:
 * `(a ~@xs b)` lowers to
 * (syntax-append (syntax-list <a>) xs (syntax-list <b>)). */
static TuriValue native_syntax_append(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        Form *p = sx_arg(a, n, i);
        if (!p || (p->tag != F_LIST && p->tag != F_NIL))
            return turi_errorf("syntax-append: part %u is %s, not a list-shaped "
                               "syntax object (a ~@ splice needs a list)",
                               i, (a[i].tag == TURI_SYNTAX && a[i].as_syntax)
                                      ? form_tag_name(a[i].as_syntax->tag)
                                      : sx_tag_name(a[i].tag));
        if (p->tag == F_LIST) total += p->as.list.len;
    }
    Form **items = (total == 0) ? NULL
        : (Form **)arena_alloc(&e->sym_arena, total * sizeof(Form *));
    uint32_t out = 0;
    Span span = SPAN_UNKNOWN;
    for (uint32_t i = 0; i < n; i++) {
        Form *p = a[i].as_syntax;
        if (p->tag != F_LIST) continue;
        if (span.line == 0 && p->span.line > 0) span = p->span;
        for (uint32_t j = 0; j < p->as.list.len; j++) items[out++] = p->as.list.items[j];
    }
    return turi_syntax_val(form_list(&e->sym_arena, span, items, total));
}

static TuriValue native_syntax_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    /* Deep structural equality, same semantics as the CT evaluator's `=`
     * (forms.c form_equal).  Named syntax=? because the `=` operator's
     * overload set is scalar-only -- precedent: sym=?. */
    Form *fa = sx_arg(a, n, 0);
    Form *fb = sx_arg(a, n, 1);
    if (!fa || !fb)
        return turi_errorf("syntax=?: expected two syntax objects");
    return turi_bool(form_equal(fa, fb));
}

static TuriValue native_syntax_gensym(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    const char *prefix = "g";
    if (n >= 1 && a[0].tag == TURI_CSTR && a[0].as_cstr) prefix = a[0].as_cstr;
    /* Same freshness contract as the elaborator's gensym_fresh: skip any
     * candidate already interned in this env's symbol table. */
    static uint32_t counter = 0;
    char name_buf[128];
    for (;;) {
        snprintf(name_buf, sizeof(name_buf), "%s_%u", prefix, counter++);
        StrSlice sl = { name_buf, (uint32_t)strlen(name_buf) };
        if (!symtab_contains(&e->st, sl)) {
            const Symbol *sym = symtab_intern(&e->st, sl);
            return turi_syntax_val(form_sym(&e->sym_arena, SPAN_UNKNOWN, sym));
        }
    }
}

static TuriValue native_syntax_error(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *msg = (n >= 1 && a[0].tag == TURI_CSTR && a[0].as_cstr)
                          ? a[0].as_cstr : "syntax error";
    Form *f = sx_arg(a, n, 1);
    if (f && f->span.line > 0)
        return turi_errorf("syntax-error at %u:%u: %s",
                           f->span.line, f->span.col_start, msg);
    return turi_errorf("syntax-error: %s", msg);
}

static void wk_register_syntax_natives(TuriEnv *env) {
    turi_env_register_native_typed(env, "read-string",     native_read_string,     NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax-tag",      native_syntax_tag,      NULL, TUR_NRT_CSTR);
    turi_env_register_native_typed(env, "syntax-len",      native_syntax_len,      NULL, TUR_NRT_INT);
    turi_env_register_native_typed(env, "syntax-first",    native_syntax_first,    NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax-rest",     native_syntax_rest,     NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax-nth",      native_syntax_nth,      NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax-list?",    native_syntax_tag_p,    (void *)(intptr_t)F_LIST,    TUR_NRT_BOOL);
    turi_env_register_native_typed(env, "syntax-vec?",     native_syntax_tag_p,    (void *)(intptr_t)F_VEC,     TUR_NRT_BOOL);
    turi_env_register_native_typed(env, "syntax-sym?",     native_syntax_tag_p,    (void *)(intptr_t)F_SYM,     TUR_NRT_BOOL);
    turi_env_register_native_typed(env, "syntax-keyword?", native_syntax_tag_p,    (void *)(intptr_t)F_KEYWORD, TUR_NRT_BOOL);
    turi_env_register_native_typed(env, "syntax-int?",     native_syntax_tag_p,    (void *)(intptr_t)F_INT,     TUR_NRT_BOOL);
    turi_env_register_native_typed(env, "syntax-float?",   native_syntax_tag_p,    (void *)(intptr_t)F_FLOAT,   TUR_NRT_BOOL);
    turi_env_register_native_typed(env, "syntax-str?",     native_syntax_tag_p,    (void *)(intptr_t)F_STR,     TUR_NRT_BOOL);
    turi_env_register_native_typed(env, "syntax-nil?",     native_syntax_tag_p,    (void *)(intptr_t)F_NIL,     TUR_NRT_BOOL);
    turi_env_register_native_typed(env, "syntax->int",     native_syntax_to_int,   NULL, TUR_NRT_INT);
    turi_env_register_native_typed(env, "syntax->float",   native_syntax_to_float, NULL, TUR_NRT_FLOAT);
    turi_env_register_native_typed(env, "syntax->str",     native_syntax_to_str,   NULL, TUR_NRT_CSTR);
    turi_env_register_native_typed(env, "syntax-sym-name", native_syntax_sym_name, NULL, TUR_NRT_CSTR);
    turi_env_register_native_typed(env, "syntax->string",  native_syntax_to_string, NULL, TUR_NRT_CSTR);
    turi_env_register_native_typed(env, "int->syntax",     native_int_to_syntax,   NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "float->syntax",   native_float_to_syntax, NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "bool->syntax",    native_bool_to_syntax,  NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "str->syntax",     native_str_to_syntax,   NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "sym->syntax",     native_sym_to_syntax,   NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax-list",     native_syntax_seq,      NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax-vec",      native_syntax_seq,      (void *)1, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax-cons",     native_syntax_cons,     NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax=?",        native_syntax_eq,       NULL, TUR_NRT_BOOL);
    turi_env_register_native_typed(env, "kw->syntax",      native_kw_to_syntax,    NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "nil->syntax",     native_nil_to_syntax,   NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax-type-ann", native_syntax_type_ann, NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax-quote",    native_syntax_quote,    NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax-append",   native_syntax_append,   NULL, TUR_NRT_SYNTAX);
    turi_env_register_native_typed(env, "syntax-gensym",   native_syntax_gensym,   NULL, TUR_NRT_SYNTAX);
    /* syntax-error never returns normally (its value is a propagating
     * error), but it types as Syntax so `(if p good-stx (syntax-error ...))`
     * unifies in a defmacro* body. */
    turi_env_register_native_typed(env, "syntax-error",    native_syntax_error,    NULL, TUR_NRT_SYNTAX);
}

void turi_env_register_interpreter_natives(TuriEnv *env) {
    if (!env) return;
    /* Register native overrides for stdlib inline-C functions. */
    wk_register_stdlib_natives(env);
    /* Contract runtime helpers: override contract.tur's inline-C
     * tur-contract-check / tur-contract-check-inv (which the simple inline-C
     * executor cannot run -- they call tur_panic) so the :pre/:post/:type
     * lowering and the assert!/require!/ensure!/invariant! macros actually
     * panic on a violated contract under --interpret. */
    turi_env_register_native(env, "tur-contract-check",
                             native_contract_check, NULL);
    turi_env_register_native(env, "tur-contract-check-inv",
                             native_contract_check_inv, NULL);
    turi_env_register_native(env, "contract-enabled?",
                             native_contract_enabled, NULL);
    /* R1 (turi-interpret-flip-residual-plan): safe.tur box/unbox/array-get/-set
     * over the int64-carrier layout, and the typeclass instance-method overrides
     * (Show/Eq inline-C bodies the tree-walker cannot run). */
    wk_register_safe_natives(env);
    wk_register_typeclass_natives(env);
    /* R1: comonad.tur Identity/Pair cell accessors (loaded on demand by user
     * code; the natives override the inline-C bodies). */
    wk_register_comonad_natives(env);
    /* R1: mutex.tur pthread handle ops (loaded on demand). */
    wk_register_mutex_natives(env);
    /* R1: future.tur refcounted FutureCell + serial.tur Bytes (loaded on demand). */
    wk_register_future_natives(env);
    wk_register_bytes_natives(env);
    /* R1: taskgroup.tur TaskGroupBlock handle ops (loaded on demand). */
    wk_register_taskgroup_natives(env);
    /* R1: chan.tur bounded sync/async channel ops (loaded on demand). */
    wk_register_chan_natives(env);
    /* R3: backtrack.tur cons-stream monad primitives (loaded on demand). */
    wk_register_backtrack_natives(env);
    /* R1: process.tur spawn/wait + fs.tur tmpfile OS-handle ops (loaded on demand). */
    wk_register_proc_fs_natives(env);
    /* R1: serial.tur Serializable int/bool instances (loaded on demand). */
    wk_register_serial_natives(env);
    /* SYM (turi): first-class :Sym ops over the auto-loaded sym.tur. */
    wk_register_sym_natives(env);
    /* SEQ (turi): lazy-Seq bridges over turi_call + generator advance.  The seq
     * modules are loaded on demand by user code; the natives override the
     * inline-C bodies. */
    wk_register_seq_natives(env);
    /* JSON (turi): tagged-AST JSON engine over layout-exact node structs,
     * overriding json.tur's malloc/recurse inline-C bodies (auto-loaded under
     * -Xjson-reader, or via an explicit (load "stdlib/json.tur")). */
    wk_register_json_natives(env);
    /* SCHEMA (turi): runtime schema validator over the JSON nodes, overriding
     * schema.tur's inline-C constructors/decoder/accessors (Layer 2). */
    wk_register_schema_natives(env);
    /* SYNTAX (turi): first-class syntax objects (TURI_SYNTAX) -- Stage 1 of
     * docs/upcoming/macro-system-direction-plan.md. */
    wk_register_syntax_natives(env);
}
