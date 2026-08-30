#ifndef TUR_LSP_DOCS_H
#define TUR_LSP_DOCS_H

#include <stddef.h>
#include "lsp_sym.h"
#include "lsp_scope.h"

/* -------------------------------------------------------------------------
 * LspDocTable -- docstring re-scanner results (LD0)
 * --------------------------------------------------------------------- */

typedef struct {
    char name[128];
    char doc[512];
} LspDocEntry;

typedef struct {
    LspDocEntry *entries;
    int          count;
    int          cap;
} LspDocTable;

void lsp_doc_table_init(LspDocTable *t);
void lsp_doc_table_free(LspDocTable *t);

/* Scan raw source text for ;;; blocks immediately preceding
 * (defn|defmacro|defstruct|definstance) forms. */
void lsp_scan_docs(const char *text, size_t len, LspDocTable *out);

/* Look up a name in the table.  Copies the doc string into out[0..cap-1].
 * Returns 1 on hit, 0 on miss. */
int lsp_scan_docs_lookup(const LspDocTable *t, const char *name,
                         char *out, size_t cap);

/* -------------------------------------------------------------------------
 * LspDoc -- per-document state
 * --------------------------------------------------------------------- */

typedef struct LspDoc {
    char   *uri;       /* heap-allocated, NUL-terminated */
    char   *path;      /* file:// decoded, heap-allocated */
    char   *text;      /* current source text, heap-allocated */
    size_t  text_len;
    /* LD1: symbol index */
    LspSymbol *symbols;
    int        symbol_count;
    int        symbol_cap;
    /* The symbol index is from an earlier revision of the text because the
     * current one did not get far enough to yield any. Not parsing is the
     * normal state while typing -- the moment the user types `(` the buffer is
     * unbalanced -- so serving a slightly stale index beats serving nothing,
     * which is what completion did before. */
    int        symbols_stale;
    /* Some revision of this text has produced a real symbol index, so there is
     * something worth retaining when a later one does not.
     *
     * Distinct from `symbols != NULL`, which is what the retention check used
     * to test and which cannot tell "analyzed, and genuinely has no symbols"
     * from "never successfully analyzed". The first analysis of a file that
     * does not parse adopted its own empty result under that test, and every
     * later failure then faithfully retained the emptiness -- so a file opened
     * with a syntax error already in it had completion dead until the error
     * was fixed unaided. */
    int        ever_analyzed;
    /* Text changed since the last analysis. Analysis is deferred until the
     * client stops typing (or asks a question that needs symbols) so a burst
     * of keystrokes costs one compile, not one per character. */
    int        dirty;
    /* S1: the lexical binding table -- locals, with the region each is
     * visible in. Collected by the same pass that fills `symbols`, and
     * adopted or retained on exactly the same rule, so the two never
     * describe different revisions of the text. */
    LspBinding *bindings;
    int         binding_count;
    int         binding_cap;
    /* The collection hit the cap. A rename refuses on this rather than
     * guessing: with a truncated table, "not a local" and "a local we had no
     * room for" are the same answer, and they call for opposite edits. */
    int         bindings_truncated;
} LspDoc;

void    lsp_docs_init(void);
void    lsp_docs_free(void);

/* Open or replace a document. Copies uri and text. */
LspDoc *lsp_doc_open(const char *uri, size_t uri_len,
                     const char *text, size_t text_len);

/* Update the text of an existing document. */
void    lsp_doc_change(const char *uri, size_t uri_len,
                       const char *text, size_t text_len);

void    lsp_doc_close(const char *uri, size_t uri_len);
LspDoc *lsp_doc_get(const char *uri, size_t uri_len);

/* Iterate every open document (read-only callback). */
void    lsp_docs_iterate(void (*cb)(const LspDoc *doc, void *ctx), void *ctx);

/* Same, but the callback may mutate the document — used to run deferred
 * analysis over whatever is dirty. */
void    lsp_docs_iterate_mut(void (*cb)(LspDoc *doc, void *ctx), void *ctx);

/* True if any open document is awaiting analysis. */
int     lsp_docs_any_dirty(void);

/* Free and zero the symbol array on doc. */
void    lsp_doc_free_symbols(LspDoc *doc);

/* Free and zero the binding table on doc. */
void    lsp_doc_free_bindings(LspDoc *doc);

/* Decode a file:// URI into a filesystem path (in-place, returns dest). */
char   *lsp_uri_to_path(const char *uri, char *dest, size_t dest_cap);

#endif
