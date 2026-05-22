#ifndef TUR_LSP_DOCS_H
#define TUR_LSP_DOCS_H

#include <stddef.h>

typedef struct LspDoc {
    char   *uri;       /* heap-allocated, NUL-terminated */
    char   *path;      /* file:// decoded, heap-allocated */
    char   *text;      /* current source text, heap-allocated */
    size_t  text_len;
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

/* Decode a file:// URI into a filesystem path (in-place, returns dest). */
char   *lsp_uri_to_path(const char *uri, char *dest, size_t dest_cap);

#endif
