#ifndef TUR_FMT_H
#define TUR_FMT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "buf.h"
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

#endif /* TUR_FMT_H */
