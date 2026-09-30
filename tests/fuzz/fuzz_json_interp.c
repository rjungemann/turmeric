/* fuzz_json_interp -- the interpreter's JSON decoder (M-2).
 *
 * The json/decode native (src/turi/interpreter_natives.c) is the twin of
 * stdlib/json.tur's compiled decoder; fuzz_json_compiled drives that one.
 * Each accepted document is freed, so a leak on any path is a finding. */
#include "fuzz_common.h"
#include "turi/interpreter_natives.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *s = fuzz_cstr(data, size);
    int64_t node = turi_json_decode_cstr(s);
    turi_json_free_tree(node);
    free(s);
    return 0;
}
