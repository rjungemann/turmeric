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

size_t lsp_offset_at_pos(const char *text, size_t text_len,
                         int line_1based, int col_1based) {
    if (!text || text_len == 0) return 0;
    size_t off = 0;
    int cur_line = 1;
    while (off < text_len && cur_line < line_1based) {
        if (text[off] == '\n') cur_line++;
        off++;
    }
    int cur_col = 1;
    while (off < text_len && text[off] != '\n' && cur_col < col_1based) {
        off++;
        cur_col++;
    }
    return off;
}

size_t lsp_prefix_at_pos(const char *text, size_t text_len,
                         int line_1based, int col_1based,
                         char *out_prefix, size_t cap) {
    if (!out_prefix || cap == 0) return 0;
    out_prefix[0] = '\0';
    if (!text || text_len == 0) return 0;

    size_t off   = lsp_offset_at_pos(text, text_len, line_1based, col_1based);
    size_t start = off;
    while (start > 0 && is_ident_char(text[start - 1])) start--;

    /* An identifier longer than the buffer is pathological; keep the head,
     * since that is the part every prefix comparison actually reads. */
    size_t len = off - start;
    if (len >= cap) len = cap - 1;
    memcpy(out_prefix, text + start, len);
    out_prefix[len] = '\0';
    return len;
}

/* One open `(` on the scan stack. */
typedef struct {
    char   name[128];  /* head symbol, "" until the first form is seen */
    int    forms;      /* forms seen at this level, head included */
    size_t last_end;   /* byte offset just past the most recent form */
} CallFrame;

#define LSP_CALL_DEPTH_MAX 64

int lsp_enclosing_call(const char *text, size_t text_len,
                       size_t cursor_off,
                       char *out_name, size_t name_cap,
                       int *active_param_out) {
    if (out_name && name_cap) out_name[0] = '\0';
    if (active_param_out) *active_param_out = 0;
    if (!text || text_len == 0) return 0;
    if (cursor_off > text_len) cursor_off = text_len;

    CallFrame stack[LSP_CALL_DEPTH_MAX];
    int depth = 0;
    /* Depth beyond the stack is still tracked as a count so the matching
     * closers pop back to a frame we do have, instead of unwinding into
     * someone else's. */
    int overflow = 0;

    size_t i = 0;
    while (i < cursor_off) {
        char c = text[i];

        if (c == ';') {                        /* comment to end of line */
            while (i < cursor_off && text[i] != '\n') i++;
            continue;
        }
        if (c == '"') {                        /* string literal is one form */
            i++;
            while (i < cursor_off && text[i] != '"') {
                if (text[i] == '\\') i++;
                i++;
            }
            if (i < cursor_off) i++;           /* past the closing quote */
            if (depth > 0 && !overflow) {
                stack[depth - 1].forms++;
                stack[depth - 1].last_end = i;
            }
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            if (depth > 0 && !overflow) stack[depth - 1].forms++;
            if (c == '(' && depth < LSP_CALL_DEPTH_MAX && !overflow) {
                stack[depth].name[0]  = '\0';
                stack[depth].forms    = 0;
                stack[depth].last_end = i + 1;
                depth++;
            } else {
                /* `[` and `{` are not calls, and a `(` past the depth cap has
                 * nowhere to live -- either way, count the nesting so the
                 * closer pops the right thing. */
                overflow++;
            }
            i++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            if (overflow > 0) overflow--;
            else if (depth > 0) {
                depth--;
                if (depth > 0) stack[depth - 1].last_end = i + 1;
            }
            i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',') {
            i++;
            continue;
        }

        /* Bare token: an identifier, number, keyword, or operator run. */
        size_t tok_start = i;
        while (i < cursor_off) {
            char t = text[i];
            if (t == ' ' || t == '\t' || t == '\n' || t == '\r' || t == ',' ||
                t == '(' || t == ')' || t == '[' || t == ']' ||
                t == '{' || t == '}' || t == '"' || t == ';')
                break;
            i++;
        }
        if (depth > 0 && !overflow) {
            CallFrame *f = &stack[depth - 1];
            if (f->forms == 0) {
                size_t n = i - tok_start;
                if (n >= sizeof(f->name)) n = sizeof(f->name) - 1;
                memcpy(f->name, text + tok_start, n);
                f->name[n] = '\0';
            }
            f->forms++;
            f->last_end = i;
        }
    }

    if (depth == 0 || overflow) return 0;
    CallFrame *f = &stack[depth - 1];
    if (!f->name[0] || f->forms == 0) return 0;

    /* A form that ends exactly at the cursor is the one being typed, so it is
     * the active argument rather than a completed one. */
    int active = f->forms - 1;
    if (f->last_end == cursor_off) active--;
    if (active < 0) {
        /* Still typing the head itself -- there is no call yet. */
        if (f->forms <= 1) return 0;
        active = 0;
    }

    if (out_name && name_cap) {
        size_t n = strlen(f->name);
        if (n >= name_cap) n = name_cap - 1;
        memcpy(out_name, f->name, n);
        out_name[n] = '\0';
    }
    if (active_param_out) *active_param_out = active;
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
