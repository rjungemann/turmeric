#ifndef TUR_CLI_LSP_LITE_H
#define TUR_CLI_LSP_LITE_H

/* `tur lsp-lite` -- minimal completion/calltip/doc helper for the Trowel
 * Turmeric plugin (and any other lightweight editor that prefers a
 * line-delimited JSON protocol over full LSP).
 *
 * Reads one JSON object per line from stdin, writes one JSON object per line
 * to stdout. See src/cli/lsp_lite.c for the request schema. */
int lsp_lite_run(int in_fd, int out_fd);

#endif
