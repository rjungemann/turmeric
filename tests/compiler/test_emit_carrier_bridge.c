/* test_emit_carrier_bridge.c -- unit tests for emit_carrier_bridge (Phase ACB).
 *
 * Covers all four conversion directions:
 *   1. CK_CARRIER -> CK_CONCRETE, inline scalar (float)
 *   2. CK_CONCRETE -> CK_CARRIER, inline scalar (float)
 *   3. CK_CARRIER -> CK_CONCRETE, pointer aggregate (struct)
 *   4. CK_CONCRETE -> CK_CARRIER, pointer aggregate (struct)
 *   5. Same-to-same: no bridge emitted (both CK_CARRIER)
 *   6. Same-to-same: no bridge emitted (both CK_CONCRETE)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "buf.h"
#include "emit_internal.h"
#include "lsp_sym.h"

static int failures = 0;

/* Stub required by emit_internal link dependencies. */
int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    (void)logical_path;
    (void)source_path; (void)out; (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

static void check_eq(const char *label, const char *got, const char *want) {
    if (got && strcmp(got, want) == 0) {
        printf("PASS [%s]\n", label);
        return;
    }
    fprintf(stderr, "FAIL [%s]: want \"%s\", got \"%s\"\n",
            label, want, got ? got : "(null)");
    failures++;
}

static void check_contains(const char *label, const char *haystack,
                            const char *needle) {
    if (haystack && strstr(haystack, needle)) {
        printf("PASS [%s] contains \"%s\"\n", label, needle);
        return;
    }
    fprintf(stderr, "FAIL [%s]: expected substring \"%s\" in \"%s\"\n",
            label, needle, haystack ? haystack : "(null)");
    failures++;
}

static void check_body_contains(const char *label, const char *body,
                                 const char *needle) {
    check_contains(label, body, needle);
}

static EmitCtx make_ctx(Buf *file, Buf *body) {
    EmitCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.file  = file;
    ctx.main_ = body;
    ctx.indent = 4;
    return ctx;
}

int main(void) {
    Arena arena;
    arena_init(&arena, 4096);

    Buf file, body;
    buf_init(&file);
    buf_init(&body);
    EmitCtx ctx = make_ctx(&file, &body);

    /* --- 1. CK_CARRIER -> CK_CONCRETE, inline float --- */
    {
        Type ft; memset(&ft, 0, sizeof(ft)); ft.kind = TY_FLOAT;
        char *r = emit_carrier_bridge(&ctx, &body, strdup("carrier_val"),
                                      CK_CARRIER, CK_CONCRETE, ft);
        check_contains("carrier->concrete float: union reinterpret", r,
                        "union { int64_t s; double d; }");
        check_contains("carrier->concrete float: .s assignment", r,
                        ".s = (carrier_val)");
        check_contains("carrier->concrete float: .d extraction", r, ".d");
        free(r);
    }

    /* --- 2. CK_CONCRETE -> CK_CARRIER, inline float --- */
    {
        buf_init(&body); /* fresh body buffer */
        Type ft; memset(&ft, 0, sizeof(ft)); ft.kind = TY_FLOAT;
        char *r = emit_carrier_bridge(&ctx, &body, strdup("1.5"),
                                      CK_CONCRETE, CK_CARRIER, ft);
        check_contains("concrete->carrier float: union reinterpret", r,
                        "union { double s; int64_t d; }");
        check_contains("concrete->carrier float: .s assignment", r, ".s = (1.5)");
        check_contains("concrete->carrier float: .d extraction", r, ".d");
        free(r);
        buf_free(&body); buf_init(&body);
    }

    /* --- 3. CK_CARRIER -> CK_CONCRETE, pointer aggregate (struct) --- */
    {
        /* Simulate a struct type: kind TY_STRUCT, type_c_name -> "MyStruct" */
        Type st; memset(&st, 0, sizeof(st)); st.kind = TY_STRUCT;
        /* type_c_name for a bare TY_STRUCT without a def returns "int64_t"
         * because def is NULL; use TY_ADT similarly.  For testing the pointer
         * dereference shape we only need a type whose carrier_is_inline is
         * false.  TY_PTR_VOID has size 8 but is not inline (not in the switch),
         * so it takes the pointer-deref path as well. */
        Type pt; memset(&pt, 0, sizeof(pt)); pt.kind = TY_PTR_VOID;
        char *r = emit_carrier_bridge(&ctx, &body, strdup("ptr_val"),
                                      CK_CARRIER, CK_CONCRETE, pt);
        check_contains("carrier->concrete ptr: pointer deref", r,
                        "(intptr_t)(ptr_val)");
        check_contains("carrier->concrete ptr: cast open", r, "*(");
        free(r);
    }

    /* --- 4. CK_CONCRETE -> CK_CARRIER, pointer aggregate --- */
    {
        buf_init(&body);
        Type pt; memset(&pt, 0, sizeof(pt)); pt.kind = TY_PTR_VOID;
        char *r = emit_carrier_bridge(&ctx, &body, strdup("struct_val"),
                                      CK_CONCRETE, CK_CARRIER, pt);
        /* Spill statement must be in body */
        check_body_contains("concrete->carrier ptr: spill decl", body.data,
                             "struct_val");
        /* Result must take address as int64_t */
        check_contains("concrete->carrier ptr: address cast", r,
                        "(int64_t)(intptr_t)(&");
        free(r);
        buf_free(&body); buf_init(&body);
    }

    /* --- 5. Same-to-same: CK_CARRIER -> CK_CARRIER (no-op) --- */
    {
        Type it; memset(&it, 0, sizeof(it)); it.kind = TY_INT;
        char *orig = strdup("x");
        char *r = emit_carrier_bridge(&ctx, &body, orig,
                                      CK_CARRIER, CK_CARRIER, it);
        check_eq("same carrier: no-op", r, "x");
        free(r);
    }

    /* --- 6. Same-to-same: CK_CONCRETE -> CK_CONCRETE (no-op) --- */
    {
        Type ft; memset(&ft, 0, sizeof(ft)); ft.kind = TY_FLOAT;
        char *orig = strdup("y");
        char *r = emit_carrier_bridge(&ctx, &body, orig,
                                      CK_CONCRETE, CK_CONCRETE, ft);
        check_eq("same concrete: no-op", r, "y");
        free(r);
    }

    buf_free(&file);
    buf_free(&body);
    arena_free(&arena);

    if (failures == 0) printf("All tests passed.\n");
    return failures ? 1 : 0;
}
