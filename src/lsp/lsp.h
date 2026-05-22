#ifndef TUR_LSP_H
#define TUR_LSP_H

/* Run the LSP server, reading JSON-RPC from fd_in and writing to fd_out.
 * Blocks until the client sends "exit". */
void lsp_server_run(int fd_in, int fd_out);

#endif
