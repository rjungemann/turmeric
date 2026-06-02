#ifndef TUR_MCP_H
#define TUR_MCP_H

/* Run the MCP (Model Context Protocol) server, reading JSON-RPC from fd_in
 * and writing to fd_out.  Blocks until the client closes the connection. */
void mcp_server_run(int fd_in, int fd_out);

#endif
