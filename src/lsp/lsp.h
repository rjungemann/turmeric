#ifndef TUR_LSP_H
#define TUR_LSP_H

#include <stdbool.h>

/* Run the LSP server, reading JSON-RPC from fd_in and writing to fd_out.
 * Blocks until the client sends "exit". */
void lsp_server_run(int fd_in, int fd_out);

/* Allow textDocument/rename on a symbol whose module the manifest `:exports`.
 *
 * Off by default. Such a name is published surface: a spice that fetched this
 * one by `:url` may import it, and those files are outside the workspace --
 * prepareRename refuses rather than producing a WorkspaceEdit that renames
 * half of an API. `tur lsp --rename-exports` says the operator knows what the
 * downstream is. */
void lsp_set_rename_exports(bool on);

#endif
