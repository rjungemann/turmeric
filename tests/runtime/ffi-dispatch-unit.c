/* tests/runtime/ffi-dispatch-unit.c -- RP2 unit test for the generated
 * FFI dispatcher table (src/runtime/ffi_dispatch.{h,c}).
 *
 * Each test picks a representative dispatcher shape and verifies that:
 *   1) The dispatcher correctly casts the supplied `void *` to the
 *      right C function-pointer type.
 *   2) Arguments are passed through unchanged.
 *   3) The return value is materialised in the expected class
 *      (int64_t register vs double register vs no-return).
 *
 * The point isn't exhaustive coverage (there are 381 dispatchers) but
 * to catch ABI regressions per signature *family*: each fundamental
 * mix of int/float args at arities 0..6 is exercised at least once.
 * Add a new case here when you tweak the dispatcher template, not when
 * you add a new shape -- the generator's determinism already guards
 * shape correctness.
 */

#include "ffi_dispatch.h"
#include "lsp_sym.h"

#include <stdint.h>
#include <stdio.h>

/* Stub: tur_core's lsp.c references this; the dispatcher test doesn't
 * touch LSP at all but the symbol still needs to resolve when we link
 * tur_core into a standalone executable. Matches the stub in
 * tests/compiler/reinterpret-codegen.c. */
int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    (void)logical_path;
    (void)source_path;
    (void)out;
    (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

static int failures = 0;
static int passes = 0;

#define EXPECT_INT(label, actual, expected) do {                          \
    int64_t _a = (actual), _e = (expected);                               \
    if (_a == _e) {                                                       \
        passes++;                                                         \
        printf("PASS %s\n", (label));                                     \
    } else {                                                              \
        failures++;                                                       \
        fprintf(stderr, "FAIL %s: expected %lld, got %lld\n",             \
                (label), (long long)_e, (long long)_a);                   \
    }                                                                     \
} while (0)

#define EXPECT_DBL(label, actual, expected) do {                          \
    double _a = (actual), _e = (expected);                                \
    /* Bit-exact for these tests -- every callee returns its inputs       \
     * verbatim or a trivial arithmetic combination. */                   \
    if (_a == _e) {                                                       \
        passes++;                                                         \
        printf("PASS %s\n", (label));                                     \
    } else {                                                              \
        failures++;                                                       \
        fprintf(stderr, "FAIL %s: expected %g, got %g\n",                 \
                (label), _e, _a);                                         \
    }                                                                     \
} while (0)

/* -------------------- callees -------------------- */

static int64_t cb_v_to_i(void) { return 0x7AAA5555AAAA5555LL; }
static double  cb_v_to_f(void) { return 1.5; }
static int     cb_v_to_v_called = 0;
static void    cb_v_to_v(void)  { cb_v_to_v_called = 1; }

static int64_t cb_i_to_i(int64_t x)            { return x + 1; }
static double  cb_i_to_f(int64_t x)            { return (double)x + 0.5; }
static int64_t cb_f_to_i(double x)             { return (int64_t)(x * 2.0); }
static double  cb_f_to_f(double x)             { return x * 3.0; }
static int     cb_i_to_v_arg = 0;
static void    cb_i_to_v(int64_t x)            { cb_i_to_v_arg = (int)x; }

static int64_t cb_ii_to_i(int64_t a, int64_t b)   { return a * b; }
static double  cb_if_to_f(int64_t a, double b)    { return (double)a + b; }
static double  cb_fi_to_f(double a, int64_t b)    { return a + (double)b; }
static double  cb_ff_to_f(double a, double b)     { return a - b; }
static int64_t cb_ff_to_i(double a, double b)     { return (int64_t)(a * b); }

static int64_t cb_iii_to_i(int64_t a, int64_t b, int64_t c) {
    return a + b * c;
}
static double  cb_iif_to_f(int64_t a, int64_t b, double c) {
    return (double)(a + b) * c;
}
static int64_t cb_fff_to_i(double a, double b, double c) {
    return (int64_t)(a + b + c);
}

static int64_t cb_iiii_to_i(int64_t a, int64_t b, int64_t c, int64_t d) {
    return a + b + c + d;
}
static double  cb_ffff_to_f(double a, double b, double c, double d) {
    return a + b + c + d;
}

static int64_t cb_iiiii_to_i(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) {
    return a * 10000 + b * 1000 + c * 100 + d * 10 + e;
}

static int64_t cb_iiiiii_to_i(int64_t a, int64_t b, int64_t c,
                              int64_t d, int64_t e, int64_t f) {
    return (((((a * 7) + b) * 7) + c) * 7 + d) * 7 + e * 7 + f;
}
static double  cb_iiiiii_to_f(int64_t a, int64_t b, int64_t c,
                              int64_t d, int64_t e, int64_t f) {
    return (double)(a + b + c + d + e + f) / 6.0;
}

/* -------------------- tests -------------------- */

int main(void) {
    /* arity 0 */
    EXPECT_INT("call_i_  (arity 0 -> int)",
               tur_ffi_call_i_((void *)cb_v_to_i),
               0x7AAA5555AAAA5555LL);
    EXPECT_DBL("call_f_  (arity 0 -> float)",
               tur_ffi_call_f_((void *)cb_v_to_f),
               1.5);
    cb_v_to_v_called = 0;
    tur_ffi_call_v_((void *)cb_v_to_v);
    EXPECT_INT("call_v_  (arity 0 -> void, side effect ran)",
               cb_v_to_v_called, 1);

    /* arity 1 */
    EXPECT_INT("call_i_i", tur_ffi_call_i_i((void *)cb_i_to_i, 41), 42);
    EXPECT_DBL("call_f_i", tur_ffi_call_f_i((void *)cb_i_to_f, 7), 7.5);
    EXPECT_INT("call_i_f", tur_ffi_call_i_f((void *)cb_f_to_i, 2.5), 5);
    EXPECT_DBL("call_f_f", tur_ffi_call_f_f((void *)cb_f_to_f, 4.0), 12.0);
    cb_i_to_v_arg = 0;
    tur_ffi_call_v_i((void *)cb_i_to_v, 99);
    EXPECT_INT("call_v_i (side effect captured arg)", cb_i_to_v_arg, 99);

    /* arity 2 -- exercises int/int, int/float, float/int, float/float */
    EXPECT_INT("call_i_ii", tur_ffi_call_i_ii((void *)cb_ii_to_i, 6, 7), 42);
    EXPECT_DBL("call_f_if", tur_ffi_call_f_if((void *)cb_if_to_f, 10, 0.25),
               10.25);
    EXPECT_DBL("call_f_fi", tur_ffi_call_f_fi((void *)cb_fi_to_f, 0.5, 99),
               99.5);
    EXPECT_DBL("call_f_ff", tur_ffi_call_f_ff((void *)cb_ff_to_f, 5.5, 1.5),
               4.0);
    EXPECT_INT("call_i_ff", tur_ffi_call_i_ff((void *)cb_ff_to_i, 3.5, 4.0), 14);

    /* arity 3 -- mixed */
    EXPECT_INT("call_i_iii",
               tur_ffi_call_i_iii((void *)cb_iii_to_i, 1, 2, 3),
               7);
    EXPECT_DBL("call_f_iif",
               tur_ffi_call_f_iif((void *)cb_iif_to_f, 3, 4, 2.0),
               14.0);
    EXPECT_INT("call_i_fff",
               tur_ffi_call_i_fff((void *)cb_fff_to_i, 1.5, 2.5, 3.0),
               7);

    /* arity 4 */
    EXPECT_INT("call_i_iiii",
               tur_ffi_call_i_iiii((void *)cb_iiii_to_i, 10, 20, 30, 40),
               100);
    EXPECT_DBL("call_f_ffff",
               tur_ffi_call_f_ffff((void *)cb_ffff_to_f, 1.0, 2.0, 3.0, 4.0),
               10.0);

    /* arity 5 (uses the stack on most ABIs -- catches a stack-passing bug) */
    EXPECT_INT("call_i_iiiii",
               tur_ffi_call_i_iiiii((void *)cb_iiiii_to_i, 5, 4, 3, 2, 1),
               54321);

    /* arity 6 -- two representative shapes, one int- and one float-return */
    EXPECT_INT("call_i_iiiiii",
               tur_ffi_call_i_iiiiii((void *)cb_iiiiii_to_i, 1, 2, 3, 4, 5, 6),
               /* (((((1*7+2)*7+3)*7+4)*7+5)*7+6 expressed via the callee. */
               ((((((1 * 7) + 2) * 7 + 3) * 7 + 4) * 7 + 5 * 7 + 6)));
    EXPECT_DBL("call_f_iiiiii",
               tur_ffi_call_f_iiiiii((void *)cb_iiiiii_to_f, 1, 2, 3, 4, 5, 6),
               (1.0 + 2.0 + 3.0 + 4.0 + 5.0 + 6.0) / 6.0);

    fflush(stdout);
    fprintf(stderr, "\nffi-dispatch-unit: %d passed, %d failed\n",
            passes, failures);
    return failures == 0 ? 0 : 1;
}
