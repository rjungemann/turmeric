/* fuzz_json_compiled -- stdlib/json.tur's decoder as compiled programs ship
 * it (M-2).  json_entry.c is `tur emit-c tests/fuzz/json_entry.tur`,
 * generated at build time; its main() is renamed so libFuzzer's can run. */
#define main tur_fuzz_entry_main
#include "json_entry.c"
#undef main

#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *s = fuzz_cstr(data, size);
    (void)fuzz_hyjson(s);
    free(s);
    return 0;
}
