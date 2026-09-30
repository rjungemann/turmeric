/* fuzz_reader -- the compiler's front door: src/compiler/reader.c.
 *
 * Under T1, `tur check`, the language server and `tur run --list` read a tree
 * the user has only opened, so the reader must not corrupt memory on any
 * source text.  The first input byte picks the reader (s-expr, curly-infix,
 * neoteric, sweet, r7rs, r7rs-sweet); the rest is the source.  Diagnostics
 * go to a sink that drops them. */
#include "fuzz_common.h"
#include "compiler/reader.h"
#include "compiler/diag.h"
#include "compiler/symbols.h"
#include "runtime/arena.h"

static void drop_diag(DiagLevel level, const char *code, const char *file,
                      uint32_t line, uint32_t col_start, uint32_t col_end,
                      const char *message, void *ud) {
    (void)level; (void)code; (void)file; (void)line;
    (void)col_start; (void)col_end; (void)message; (void)ud;
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    /* Debug ASan builds quarantine arena slabs forever (arena.c); a fuzz loop
     * frees an arena per input, so turn the quarantine off. */
    setenv("TUR_DEBUG_ARENA_POISON", "0", 1);
    diag_set_sink(drop_diag, NULL);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static const ReaderType kinds[] = {
        READER_TURMERIC, READER_CURLY_INFIX, READER_NEOTERIC,
        READER_SWEET, READER_R7RS, READER_R7RS_SWEET,
    };
    if (size == 0) return 0;
    ReaderType rt = kinds[data[0] % (sizeof kinds / sizeof kinds[0])];
    char *src = fuzz_cstr(data + 1, size - 1);

    diag_reset();
    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);
    SourceFile file;
    memset(&file, 0, sizeof file);
    file.path = "<fuzz>";
    file.base_dir = "/nonexistent-fuzz-dir";
    file.src = src;
    file.len = size - 1;
    file.file_id = 0;
    file.reader_type = rt;
    diag_register_file(&file);
    uint32_t n = 0;
    (void)read_all(&arena, &st, &file, &n);
    diag_reset();
    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return 0;
}
