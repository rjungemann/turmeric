/* test_kind_row.c -- unit tests for the kind-level `List Type` (KIND_TYPEROW).
 *
 * KIND_TYPEROW (0xFFFE) is the kind of a *row of types* -- the `[Pos Vel]`
 * component row of an ECS Query, a relation's column tuple, a data-frame
 * schema. It is the first elaborator-level building block of the variadic-HKT-
 * rows work tracked in docs/archive/history/variadic-hkt-rows-missing.md (Direction 1).
 *
 * Like KIND_ROW (the effect-row sentinel), KIND_TYPEROW is *not* an arrow kind:
 * a row is a first-class kind, not a constructor you apply. These tests pin the
 * parse/print round-trip and the apply/unify algebra so the sentinel cannot be
 * confused with an arity-N arrow kind or with the effect-row sentinel.
 */

#include <stdio.h>
#include <string.h>

#include "types.h"
#include "kind_check.h"
#include "diag.h"
#include "lsp_sym.h"

static int failures = 0;

/* Stub required by the tur_core link dependency (mirrors the other
 * compiler-internals unit tests). */
int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    (void)logical_path;
    (void)source_path; (void)out; (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

static void check_str(const char *label, const char *got, const char *want) {
    if (got && strcmp(got, want) == 0) {
        printf("PASS [%s]\n", label);
        return;
    }
    fprintf(stderr, "FAIL [%s]: want \"%s\", got \"%s\"\n",
            label, want, got ? got : "(null)");
    failures++;
}

static void check_kind(const char *label, Kind got, Kind want) {
    if (got == want) {
        printf("PASS [%s]\n", label);
        return;
    }
    fprintf(stderr, "FAIL [%s]: want %u, got %u\n",
            label, (unsigned)want, (unsigned)got);
    failures++;
}

int main(void) {
    Span sp = (Span){0};

    /* --- print round-trip --- */
    check_str("kind_to_string(KIND_TYPEROW) == \"[*]\"",
              kind_to_string(KIND_TYPEROW), "[*]");
    /* The type-row sentinel must NOT collide with the effect-row sentinel. */
    check_str("kind_to_string(KIND_ROW) == \"Row\"",
              kind_to_string(KIND_ROW), "Row");

    /* --- parse round-trip --- */
    check_kind("kind_parse(\"[*]\") == KIND_TYPEROW",
               kind_parse("[*]"), KIND_TYPEROW);
    check_kind("kind_parse(kind_to_string(KIND_TYPEROW)) round-trips",
               kind_parse(kind_to_string(KIND_TYPEROW)), KIND_TYPEROW);
    /* Garbled row syntax falls back to KIND_STAR, never silently to TYPEROW. */
    check_kind("kind_parse(\"[*\") == KIND_STAR",  kind_parse("[*"),  KIND_STAR);
    check_kind("kind_parse(\"[**]\") == KIND_STAR", kind_parse("[**]"), KIND_STAR);

    /* --- the sentinels are distinct values --- */
    if (KIND_TYPEROW == KIND_ROW || KIND_TYPEROW == KIND_STAR) {
        fprintf(stderr, "FAIL [sentinels distinct]: TYPEROW=%u ROW=%u STAR=%u\n",
                (unsigned)KIND_TYPEROW, (unsigned)KIND_ROW, (unsigned)KIND_STAR);
        failures++;
    } else {
        printf("PASS [sentinels distinct]\n");
    }

    /* --- apply algebra: a row is inert under application (identity), exactly
     *     like KIND_ROW and unlike any arrow kind which steps down a rung. --- */
    check_kind("kind_apply_one(KIND_TYPEROW) == KIND_TYPEROW (identity)",
               kind_apply_one(KIND_TYPEROW), KIND_TYPEROW);
    check_kind("kind_apply_one(KIND_ARROW) == KIND_STAR (steps down)",
               kind_apply_one(KIND_ARROW), KIND_STAR);

    /* --- kind_of_type_app: applying anything to a row is a kind error --- */
    Type row_fn = type_simple(TY_INT, CK_COPY); /* any Type; we override hkt_kind */
    row_fn.hkt_kind = KIND_TYPEROW;
    Type arg = type_simple(TY_INT, CK_COPY);
    /* This emits TUR-E0012 (cannot apply a non-constructor kind) and yields
     * KIND_STAR. Capture the (expected) diagnostic so the test log stays clean,
     * and assert that exactly one error was raised. */
    diag_push_capture();
    check_kind("kind_of_type_app(row, _) == KIND_STAR (rejects application)",
               kind_of_type_app(row_fn, arg, sp), KIND_STAR);
    check_kind("kind_of_type_app(row, _) raised exactly one diagnostic",
               (Kind)diag_pop_capture(), (Kind)1);

    /* --- unify: TYPEROW with itself is TYPEROW (no diagnostic) ... --- */
    check_kind("kind_unify(TYPEROW, TYPEROW) == TYPEROW",
               kind_unify(KIND_TYPEROW, KIND_TYPEROW, sp), KIND_TYPEROW);

    /* ... and TYPEROW vs STAR / ARROW / ROW is a mismatch: returns KIND_STAR and
     *     emits one TUR-E0012 each (captured). A row must never silently unify
     *     with a concrete type, an arrow constructor, or the effect-row sentinel. */
    diag_push_capture();
    check_kind("kind_unify(TYPEROW, STAR) == KIND_STAR (mismatch)",
               kind_unify(KIND_TYPEROW, KIND_STAR, sp), KIND_STAR);
    check_kind("kind_unify(TYPEROW, ARROW) == KIND_STAR (mismatch)",
               kind_unify(KIND_TYPEROW, KIND_ARROW, sp), KIND_STAR);
    check_kind("kind_unify(TYPEROW, ROW) == KIND_STAR (mismatch vs effect-row)",
               kind_unify(KIND_TYPEROW, KIND_ROW, sp), KIND_STAR);
    check_kind("the three TYPEROW mismatches raised three diagnostics",
               (Kind)diag_pop_capture(), (Kind)3);

    if (failures == 0) {
        printf("\nAll kind-row tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d kind-row test(s) FAILED.\n", failures);
    return 1;
}
