#ifndef TUR_LSP_DOCS_H
#define TUR_LSP_DOCS_H

#include <stddef.h>
#include "lsp_sym.h"

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

/* Free and zero the symbol array on doc. */
void    lsp_doc_free_symbols(LspDoc *doc);

/* Decode a file:// URI into a filesystem path (in-place, returns dest). */
char   *lsp_uri_to_path(const char *uri, char *dest, size_t dest_cap);

#endif
