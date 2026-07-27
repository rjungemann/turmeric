#ifndef TUR_FMT_H
#define TUR_FMT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "buf.h"
#include "diag.h"    /* ReaderType */
#include "forms.h"

typedef struct FmtOptions {
    uint32_t    indent_width;       /* spaces per indent level (default 2) */
    uint32_t    line_width;         /* max line length before wrapping (default 80) */
    bool        align_let_bindings; /* align binding names in let blocks */
    /* Original source text for comment re-extraction (may be NULL).
     * When provided, comments between forms are preserved in output. */
    const char *src;
    size_t      src_len;
} FmtOptions;

/* Format all top-level forms in `forms` and write the result into `buf`.
 * Returns 0 on success, -1 on failure. */
int fmt_print(Buf *buf, Form **forms, uint32_t count, FmtOptions opts);

/* Read `src` as `rtype` and format it -- the whole `tur fmt` pipeline over an
 * in-memory buffer, with no filesystem read.
 *
 * `path_label` names the buffer in any diagnostic; it does not have to exist
 * on disk. `rtype` selects the dialect (the CLI derives it from the file
 * extension via reader_type_from_extension).
 *
 * Lives here rather than in main.c because the LSP's textDocument/formatting
 * handler needs it too, and the LSP is linked into tur_core -- which main.c is
 * not part of.
 *
 * On success returns 0 with *out populated (buf_init'd here; the caller frees
 * it). On failure returns -1 and *out is untouched -- do not free it. */
int fmt_format_buffer(const char *path_label, const char *src, size_t len,
                      ReaderType rtype, Buf *out);

#endif /* TUR_FMT_H */
