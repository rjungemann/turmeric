#include "lsp_util.h"

#include <string.h>
#include <stdio.h>

static int is_ident_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '?' || c == '!' || c == '*' ||
           c == '/' || c == '>' || c == '<' || c == '+' ||
           c == '=' || c == '_';
}

int lsp_word_at_pos(const char *text, size_t text_len,
                    int line_1based, int col_1based,
                    char *out_name, size_t name_cap) {
    if (!text || text_len == 0 || !out_name || name_cap == 0) return 0;
    out_name[0] = '\0';

    /* Walk to the start of the requested line */
    const char *p   = text;
    const char *end = text + text_len;
    int cur_line = 1;
    while (p < end && cur_line < line_1based) {
        if (*p == '\n') cur_line++;
        p++;
    }
    if (p >= end) return 0;

    /* Advance to the requested column (1-based) */
    const char *line_start = p;
    int cur_col = 1;
    while (p < end && *p != '\n' && cur_col < col_1based) {
        p++;
        cur_col++;
    }
    if (p >= end || *p == '\n') return 0;

    /* If cursor is not on an identifier character, try scanning right one step
     * (handles "cursor is just before the word" edge case). */
    if (!is_ident_char(*p)) {
        if (p + 1 < end && is_ident_char(p[1]))
            p++;
        else
            return 0;
    }

    /* Scan left to find word start */
    const char *word_start = p;
    while (word_start > line_start && is_ident_char(*(word_start - 1)))
        word_start--;

    /* Scan right to find word end */
    const char *word_end = p;
    while (word_end < end && *word_end != '\n' && is_ident_char(*word_end))
        word_end++;

    size_t len = (size_t)(word_end - word_start);
    if (len == 0 || len >= name_cap) return 0;

    memcpy(out_name, word_start, len);
    out_name[len] = '\0';
    return 1;
}

char *lsp_path_to_uri(const char *path, char *dest, size_t dest_cap) {
    if (!path || !dest || dest_cap < 8) return dest;
    size_t di = 0;
    /* Prefix */
    const char *prefix = "file://";
    while (*prefix && di < dest_cap - 1) dest[di++] = *prefix++;
    /* Encode path: percent-encode characters outside unreserved + '/' */
    for (const char *p = path; *p && di + 3 < dest_cap; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            dest[di++] = (char)c;
        } else {
            di += (size_t)snprintf(dest + di, dest_cap - di, "%%%02X", c);
        }
    }
    dest[di] = '\0';
    return dest;
}
