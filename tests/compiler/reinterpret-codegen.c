/* reinterpret-codegen.c -- synthetic codegen test for EX_REINTERPRET. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "buf.h"
#include "emit_internal.h"
#include "lsp_sym.h"

static int failures = 0;

int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    (void)logical_path;
    (void)source_path;
    (void)out;
    (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

static void check_contains(const char *label, const char *haystack, const char *needle) {
    if (strstr(haystack, needle)) {
        printf("PASS [%s] contains %s\n", label, needle);
        return;
    }
    fprintf(stderr, "FAIL [%s]: expected substring %s\n", label, needle);
    failures++;
}

int main(void) {
    Arena arena;
    arena_init(&arena, 4096);

    Expr *f = expr_new(&arena, EX_FLOAT_LIT, TYPE_FLOAT, SPAN_UNKNOWN);
    f->as.f = 1.5;

    Expr *to_int = expr_new(&arena, EX_REINTERPRET, TYPE_INT, SPAN_UNKNOWN);
    to_int->as.reinterpret_.expr = f;
    to_int->as.reinterpret_.source_kind = TY_FLOAT;
    to_int->as.reinterpret_.target_kind = TY_INT;

    Expr *back_to_float = expr_new(&arena, EX_REINTERPRET, TYPE_FLOAT, SPAN_UNKNOWN);
    back_to_float->as.reinterpret_.expr = to_int;
    back_to_float->as.reinterpret_.source_kind = TY_INT;
    back_to_float->as.reinterpret_.target_kind = TY_FLOAT;

    Buf file;
    Buf body;
    EmitCtx ctx;
    buf_init(&file);
    buf_init(&body);
    memset(&ctx, 0, sizeof(ctx));
    ctx.file = &file;
    ctx.main_ = &body;
    ctx.indent = 4;

    char *emitted = emit_value(&ctx, &body, back_to_float);
    if (!emitted) {
        fprintf(stderr, "FAIL [reinterpret-codegen]: emit_value returned NULL\n");
        failures++;
    } else {
        check_contains("reinterpret-codegen", emitted, "union { int64_t s; double d; }");
        check_contains("reinterpret-codegen", emitted, "union { double s; int64_t d; }");
        check_contains("reinterpret-codegen", emitted, ".s = 1.5");
        free(emitted);
    }

    buf_free(&file);
    buf_free(&body);
    arena_free(&arena);

    return failures ? 1 : 0;
}
