#ifndef TUR_LSP_JSON_H
#define TUR_LSP_JSON_H

#include <stddef.h>
#include <stdint.h>

/* Extract a string value for a top-level key in a flat JSON object.
 * Returns pointer into `json` (not NUL-terminated); fills *out_len.
 * Returns NULL if the key is absent. */
const char *lsp_json_str(const char *json, const char *key, size_t *out_len);

/* Extract a 64-bit integer value for a top-level key.
 * Returns -1 if absent or not a number. */
int64_t lsp_json_int(const char *json, const char *key);

/* Return a pointer to the value of `key` as a raw JSON substring.
 * Useful for nested objects. Fills *out_len with the length of the raw value.
 * Returns NULL if absent. */
const char *lsp_json_raw(const char *json, const char *key, size_t *out_len);

/* Copy a string value into dest (NUL-terminated, truncated to dest_cap-1).
 * Returns 0 on success, -1 if key absent. */
int lsp_json_str_copy(const char *json, const char *key,
                      char *dest, size_t dest_cap);

#endif
