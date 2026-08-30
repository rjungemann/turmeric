/* test_kind_arity.c -- unit tests for the arbitrary-arity Kind helpers
 * (Theme E / Phase TP3).
 *
 * The arity-5 cap on Kind is gone: Kind is `typedef uint16_t Kind` with the
 * encoding 0 -> KIND_STAR, 1..N -> arity-N arrow kind, 0xFFFF -> KIND_ROW.
 * These tests pin the round-trip and algebra of the four kind helpers across
 * arities well past the old cap (through arity 8 and 16), so a regression in
 * kind_parse / kind_to_string / kind_for_arity / kind_apply_one is caught
 * without depending on the codegen snapshot harness.
 */

#include <stdio.h>
#include <string.h>

#include "types.h"
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

/* Build the canonical "* -> * -> ... -> *" string for a given arity. */
static void build_arrow_string(uint32_t arity, char *out, size_t cap) {
    out[0] = '\0';
    for (uint32_t i = 0; i < arity; i++) {
        strncat(out, "* -> ", cap - strlen(out) - 1);
    }
    strncat(out, "*", cap - strlen(out) - 1);
}

int main(void) {
    /* --- kind_for_arity is the identity on the arity, no cap --- */
    check_kind("kind_for_arity(0) == KIND_STAR", kind_for_arity(0), KIND_STAR);
    check_kind("kind_for_arity(1) == KIND_ARROW", kind_for_arity(1), KIND_ARROW);
    check_kind("kind_for_arity(5) == KIND_ARROW5", kind_for_arity(5), KIND_ARROW5);
    check_kind("kind_for_arity(6) past old cap", kind_for_arity(6), (Kind)6);
    check_kind("kind_for_arity(8)", kind_for_arity(8), (Kind)8);
    check_kind("kind_for_arity(16)", kind_for_arity(16), (Kind)16);

    /* --- kind_to_string / kind_parse round-trip across arities ---
     * kind_to_string memoises canonical strings for arities 0..15 (the
     * common case, zero-alloc, const char *). Within that range the
     * printed string must round-trip exactly back to the same Kind. */
    for (uint32_t arity = 0; arity <= 15; arity++) {
        char want[128];
        char label[64];
        build_arrow_string(arity, want, sizeof(want));

        Kind k = kind_for_arity(arity);
        const char *s = kind_to_string(k);
        snprintf(label, sizeof(label), "kind_to_string(arity=%u)", arity);
        check_str(label, s, want);

        /* Parse the printed string back; must recover the same Kind. */
        Kind back = kind_parse(want);
        snprintf(label, sizeof(label), "kind_parse round-trip(arity=%u)", arity);
        check_kind(label, back, k);
    }

    /* --- Beyond the memoised table (arity >= 16): kind_to_string falls
     * back to a diagnostic form (not the canonical arrow string), but
     * kind_parse is unbounded and still recovers the exact Kind from a
     * canonical string. This pins the documented boundary. */
    {
        char want[256];
        build_arrow_string(16, want, sizeof(want));
        Kind back = kind_parse(want);
        check_kind("kind_parse unbounded(arity=16)", back, kind_for_arity(16));
        /* The fallback is non-NULL and clearly not the canonical string. */
        const char *s = kind_to_string(kind_for_arity(16));
        if (s && strcmp(s, want) != 0) {
            printf("PASS [kind_to_string(arity=16) falls back to diagnostic]\n");
        } else {
            fprintf(stderr, "FAIL [kind_to_string(arity=16) fallback]: got \"%s\"\n",
                    s ? s : "(null)");
            failures++;
        }
    }

    /* --- kind_apply_one decrements arrow arity, fixed at STAR/ROW --- */
    check_kind("apply(STAR) == STAR", kind_apply_one(KIND_STAR), KIND_STAR);
    check_kind("apply(ROW) == ROW", kind_apply_one(KIND_ROW), KIND_ROW);
    check_kind("apply(arity 8) == arity 7",
               kind_apply_one(kind_for_arity(8)), kind_for_arity(7));
    /* Fully saturate an arity-8 kind one application at a time. */
    {
        Kind k = kind_for_arity(8);
        for (int i = 0; i < 8; i++) k = kind_apply_one(k);
        check_kind("apply x8 saturates arity-8 to STAR", k, KIND_STAR);
    }

    /* --- KIND_ROW is distinct from every arrow kind --- */
    check_str("kind_to_string(KIND_ROW)", kind_to_string(KIND_ROW), "Row");

    if (failures == 0) printf("All tests passed.\n");
    return failures ? 1 : 0;
}
