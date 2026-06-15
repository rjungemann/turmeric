/* test_heap_abi.c -- unit tests for the :heap typed-pointer ABI lowering
 * (end-to-end-monomorphization, M3->M7 Vec typed-pointer vertical slice, step 1).
 *
 * Pins type_c_name's behaviour for a StructDef carrying the new `is_heap` flag,
 * in isolation from the full codegen path:
 *
 *   - a NON-heap struct lowers to its bare struct name (the by-value ABI);
 *   - a :heap struct lowers to a typed pointer `Name *`;
 *   - flipping is_heap on the same def flips the lowering (proving the flag is
 *     the sole gate -- it is inert when unset, which is what keeps the whole
 *     fixture suite byte-identical until a real type is tagged).
 *
 * This is the unit pin the vertical-slice plan's sub-step 1 calls for: it does
 * not exercise construction / field access / free (later sub-steps), only the
 * type -> C-name lowering decision the flag controls.
 */

#include <stdio.h>
#include <string.h>

#include "types.h"
#include "lsp_sym.h"

static int failures = 0;

/* Stub required by the tur_core link dependency (mirrors the other
 * compiler-internals unit tests). */
int tur_collect_symbols(const char *source_path, LspSymbol *out, int cap,
                        int *count_out) {
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

int main(void) {
    /* A minimal non-parameterized StructDef. With is_heap unset it must lower to
     * the bare name (the by-value ABI, unchanged from before this flag). */
    StructDef def;
    memset(&def, 0, sizeof(def));
    def.name = "HBox";
    def.n_fields = 0;
    def.fields = NULL;
    def.n_type_params = 0;

    Type t = type_struct(&def);

    check_str("non-heap struct lowers to bare name",
              type_c_name(t), "HBox");

    /* Flip the flag: the SAME def must now lower to a typed pointer. This proves
     * is_heap is the only gate, so the lowering is inert until a type is tagged. */
    def.is_heap = true;
    check_str("heap struct lowers to typed pointer",
              type_c_name(t), "HBox *");

    /* Flip back: lowering returns to the by-value name (no sticky state). */
    def.is_heap = false;
    check_str("clearing is_heap restores by-value name",
              type_c_name(t), "HBox");

    if (failures == 0) printf("All tests passed.\n");
    return failures ? 1 : 0;
}
