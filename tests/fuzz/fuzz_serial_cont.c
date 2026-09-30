/* fuzz_serial_cont -- the serialized-continuation deserializer compiled
 * programs ship (M-1): tur_serial_cont_check, then (for a buffer it accepts)
 * tur_serial_cont_deserialize.  serial_entry.c is `tur emit-c
 * tests/fuzz/serial_entry.tur`, whose static initializer registers the call
 * frames the buffers can name.
 *
 * The rebuilt chain is freed, cstr envs included, but never RESUMED: resuming
 * runs the program's own frames (a divide frame on a zero env aborts, by
 * design), and the rebuild is what reads the untrusted bytes. */
#define main tur_fuzz_entry_main
#include "serial_entry.c"
#undef main

#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The in-memory `bytes` layout: an int64 length, then the payload. */
    int64_t *b = (int64_t *)malloc(sizeof(int64_t) + size);
    if (!b) abort();
    b[0] = (int64_t)size;
    if (size) memcpy(b + 1, data, size);
    if (tur_serial_cont_check((int64_t)(intptr_t)b) == 0) {
        DK *k = (DK *)(intptr_t)tur_serial_cont_deserialize((int64_t)(intptr_t)b);
        for (DK *q = k; q && q->kind == DKK_FRAME; q = q->next) {
            SkReg *r = __sk_reg_for_frame(q->fn);
            if (r && r->env_kind == SK_ENV_CSTR) free((void *)q->env);
        }
        dk_free(k);
    }
    free(b);
    return 0;
}
