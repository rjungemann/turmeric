/*
 * completion.h -- `tur completion <zsh|bash>`: emit a shell completion script.
 */
#ifndef TUR_CLI_COMPLETION_H
#define TUR_CLI_COMPLETION_H

/* Print the completion script for argv[2] ("zsh" or "bash") to stdout.
 * Returns 0 on success, 2 on a CLI error (message already printed). */
int cmd_completion(int argc, char **argv);

/* Print `tur completion` usage to stderr; always returns 0. */
int usage_completion(void);

#endif /* TUR_CLI_COMPLETION_H */
