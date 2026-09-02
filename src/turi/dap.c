/* dap.c -- Debug Adapter Protocol server for the Turmeric interpreter.
 *
 * Implements the DAP over JSON-RPC 2.0 / stdio (Content-Length framing).  Start
 * with:  tur dap
 *
 * It is a thin shell around the Phase 2 interpreter debugger (src/turi/eval.c):
 * the debugger does the real work (breakpoints, stepping, the call stack, locals
 * inspection) and this file maps DAP requests onto its Phase 3 control API
 * (turi_debug_* in eval.h).
 *
 * Lifecycle:
 *   initialize        -> capabilities + `initialized` event
 *   setBreakpoints    -> staged (pre-launch) or applied live (while paused)
 *   launch            -> record program / args / stopOnEntry
 *   configurationDone -> run the program (blocking; pauses drive the stops)
 *   ...stops...       -> `stopped` event; stackTrace / scopes / variables /
 *                        evaluate served from the paused frame
 *   continue / next / stepIn / stepOut -> resume
 *   program exit      -> `exited` + `terminated`
 *   disconnect        -> tear down
 *
 * Replay mode (editor-intelligence follow-through, T2): `launch` with
 * `"replay": true` inverts that. The program runs to completion with the
 * recorder attached (turi/trace.c) and the session is then served from the
 * recording -- so stackTrace, scopes and variables answer from a trace cursor
 * rather than a live frame, and `stepBack` / `reverseContinue` / `reverseNext`
 * are answerable at all. VS Code and nvim-dap draw the whole reverse-execution
 * UI off supportsStepBack, so there is no widget to write here. The one thing
 * a recording cannot do is `evaluate`: there is no live frame to evaluate in,
 * and it says so rather than returning something stale.
 *
 * A recording is also an axis, which DAP has no vocabulary for. Three custom
 * requests add one -- `replayInfo` (how long, where are we), `replaySeek` (go
 * to step N) and `replaySites` (where steps are and how deep, by index or
 * downsampled) -- plus a `replayOutput` event for the case a delta cannot
 * express: a backwards seek shortens the transcript. Together they are what a
 * timeline scrubber and a depth ribbon need; a client detects them from
 * `supportsTurmericReplayTimeline`, and one that does not know them still gets
 * exactly the session it got before.
 *
 * Shapes follow Try Turmeric, which built this timeline first: `web/main.js`
 * (the seek loop and its coalescing), `web/public/eval-worker.js`
 * (`trace-seek` / `trace-site-at`) and `src/web/wasm_glue.c`
 * (`turi_wasm_trace_state` / `_site_at` / `_output_full`). Read those before
 * changing anything here -- every question this file answers, that one has
 * answered once already, and in-process where the answers are cheaper.
 *
 * See docs/archive/history/debugger-plan.md (Phase 3) and
 * docs/artifacts/debugger-dap-phase3.md.
 */

#include "dap.h"
#include "eval.h"
#include "trace.h"

#include "../lsp/lsp_io.h"
#include "../lsp/lsp_json.h"
#include "buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* ------------------------------------------------------------------------- */

#define DAP_MAX_BP 256

typedef struct {
    char     file[256];   /* basename matched against node source files */
    uint32_t line;
    char     cond[160];   /* "" = unconditional */
} DapBp;

typedef struct DapState {
    int       rpc_fd;        /* JSON-RPC out (the saved real stdout) */
    int       in_fd;         /* JSON-RPC in */
    int       seq;           /* outgoing message sequence */
    TuriEnv  *env;           /* set in dap_begin_session; NULL pre-launch */
    bool      stop_on_entry;
    int       out_pipe_r;    /* read end of the debuggee-stdout capture pipe */
    /* Breakpoints staged by setBreakpoints before the env exists. */
    DapBp     bps[DAP_MAX_BP];
    int       n_bps;

    /* T2: replay mode.
     *
     * `launch` with `"replay": true` records the whole run first and then
     * serves the session from the recording. stackTrace / scopes / variables
     * answer from the trace cursor rather than from a live env, which is what
     * makes stepping BACKWARDS possible at all -- a pause cannot go back.
     *
     * Off by default: the live session is the one that can `evaluate`, and a
     * recording has no frame to evaluate in. */
    bool             replay_mode;
    TurTrace        *trace;
    TurTraceReplay  *replay;
    /* How much of the recorded output the client has been told about.
     *
     * Forward motion appends via `output`. Backward motion cannot: the new
     * transcript is a prefix of the old one and a delta cannot express a
     * truncation, so it re-sends the whole thing as `replayOutput`. This
     * therefore tracks the client's view, not a high-water mark -- it goes
     * down as well as up. */
    size_t           replay_out_sent;
} DapState;

/* ---------------------------------------------------------------------------
 * JSON helpers
 * --------------------------------------------------------------------------- */

static void dap_json_escape_n(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n");  break;
        case '\r': buf_puts(b, "\\r");  break;
        case '\t': buf_puts(b, "\\t");  break;
        default:
            if (c < 0x20) buf_printf(b, "\\u%04x", c);
            else          buf_putc(b, (char)c);
        }
    }
}

static void dap_json_escape(Buf *b, const char *s) {
    if (s) dap_json_escape_n(b, s, strlen(s));
}

static bool dap_json_bool(const char *json, const char *key) {
    size_t l;
    const char *v = lsp_json_raw(json, key, &l);
    return v && l >= 4 && strncmp(v, "true", 4) == 0;
}

/* Iterate `{...}` objects inside a JSON array string.  Returns a pointer to the
 * next object's opening brace (or NULL at the end) and advances *cur past it.
 * The returned pointer is suitable for lsp_json_* (they stop at the matching
 * brace, so no NUL-termination of the object is required). */
static const char *dap_array_next_obj(const char **cur) {
    const char *p = *cur;
    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') { *cur = p; return NULL; }
    const char *start = p;
    int depth = 0;
    while (*p) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') { if (*p == '\\') p++; if (*p) p++; }
            if (*p) p++;
            continue;
        }
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
        p++;
    }
    *cur = p;
    return start;
}

/* Parse a JSON array of strings (e.g. launch "args") into a malloc'd argv. */
static int dap_parse_str_array(const char *json, const char *key, char ***out) {
    *out = NULL;
    size_t l;
    const char *arr = lsp_json_raw(json, key, &l);
    if (!arr) return 0;
    char **v = NULL;
    int n = 0, cap = 0;
    const char *p = arr;
    while (*p && *p != ']') {
        if (*p == '"') {
            p++;
            const char *s = p;
            while (*p && *p != '"') { if (*p == '\\') p++; if (*p) p++; }
            size_t len = (size_t)(p - s);
            char *str = (char *)malloc(len + 1);
            memcpy(str, s, len);
            str[len] = '\0';
            if (n == cap) { cap = cap ? cap * 2 : 4; v = (char **)realloc(v, (size_t)cap * sizeof *v); }
            v[n++] = str;
            if (*p) p++;
        } else {
            p++;
        }
    }
    *out = v;
    return n;
}

static const char *dap_basename(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* ---------------------------------------------------------------------------
 * Outgoing message framing
 * --------------------------------------------------------------------------- */

static void dap_write(DapState *s, Buf *b) {
    lsp_write_message(s->rpc_fd, b->data, b->len);
}

/* NUL-terminate a Buf in place (without counting the terminator in len) so its
 * `data` can be passed as a C string -- Buf does not maintain a trailing NUL
 * across buf_puts/buf_putc. */
static const char *dap_cstr(Buf *b) {
    buf_putc(b, '\0');
    b->len--;
    return b->data;
}

static void dap_send_response(DapState *s, int64_t req_seq, const char *cmd,
                              const char *body, bool success) {
    Buf b; buf_init(&b);
    buf_printf(&b,
        "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%lld,"
        "\"success\":%s,\"command\":\"%s\"",
        s->seq++, (long long)req_seq, success ? "true" : "false", cmd);
    if (body) buf_printf(&b, ",\"body\":%s", body);
    buf_putc(&b, '}');
    dap_write(s, &b);
    buf_free(&b);
}

static void dap_send_error(DapState *s, int64_t req_seq, const char *cmd,
                           const char *message) {
    Buf b; buf_init(&b);
    buf_printf(&b,
        "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%lld,"
        "\"success\":false,\"command\":\"%s\",\"message\":\"",
        s->seq++, (long long)req_seq, cmd);
    dap_json_escape(&b, message);
    buf_puts(&b, "\"}");
    dap_write(s, &b);
    buf_free(&b);
}

static void dap_send_event(DapState *s, const char *event, const char *body) {
    Buf b; buf_init(&b);
    buf_printf(&b, "{\"seq\":%d,\"type\":\"event\",\"event\":\"%s\"",
               s->seq++, event);
    if (body) buf_printf(&b, ",\"body\":%s", body);
    buf_putc(&b, '}');
    dap_write(s, &b);
    buf_free(&b);
}

/* ---------------------------------------------------------------------------
 * Debuggee stdout capture
 * --------------------------------------------------------------------------- */

/* Drain whatever the debuggee has written to the capture pipe and forward it as
 * `output` events.  Non-blocking, so safe to call at any pause. */
static void dap_drain_output(DapState *s) {
    if (s->out_pipe_r < 0) return;
    fflush(stdout);
    char buf[4096];
    for (;;) {
        ssize_t n = read(s->out_pipe_r, buf, sizeof buf);
        if (n <= 0) break;   /* EAGAIN (nonblocking) or EOF */
        Buf b; buf_init(&b);
        buf_printf(&b, "{\"seq\":%d,\"type\":\"event\",\"event\":\"output\","
                       "\"body\":{\"category\":\"stdout\",\"output\":\"",
                   s->seq++);
        dap_json_escape_n(&b, buf, (size_t)n);
        buf_puts(&b, "\"}}");
        dap_write(s, &b);
        buf_free(&b);
        if (n < (ssize_t)sizeof buf) break;
    }
}

/* ---------------------------------------------------------------------------
 * Conditional-breakpoint evaluation
 * --------------------------------------------------------------------------- */

static void dap_rewrite_condition(const char *cond, char *out, size_t cap) {
    /* Check if it's already a Lisp-style expression starting with '(' */
    const char *p = cond;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '(') {
        snprintf(out, cap, "%s", cond);
        return;
    }

    /* Standard DAP/C-style expression: name op literal */
    int op_len = 0;
    const char *op = strstr(cond, "==");
    if (op) { op_len = 2; }
    else {
        op = strstr(cond, "!=");
        if (op) { op_len = 2; }
        else {
            op = strstr(cond, "<=");
            if (op) { op_len = 2; }
            else {
                op = strstr(cond, ">=");
                if (op) { op_len = 2; }
                else {
                    op = strchr(cond, '<');
                    if (op) { op_len = 1; }
                    else {
                        op = strchr(cond, '>');
                        if (op) { op_len = 1; }
                    }
                }
            }
        }
    }

    if (!op) {
        /* No operator found, evaluate as-is */
        snprintf(out, cap, "%s", cond);
        return;
    }

    char lhs[128], rhs[128], op_str[8];
    size_t llen = (size_t)(op - cond);
    if (llen >= sizeof lhs) llen = sizeof lhs - 1;
    memcpy(lhs, cond, llen); lhs[llen] = '\0';
    snprintf(rhs, sizeof rhs, "%s", op + op_len);
    
    /* Trim spaces */
    size_t ln = strlen(lhs);
    while (ln > 0 && (lhs[ln - 1] == ' ' || lhs[ln - 1] == '\t')) lhs[--ln] = '\0';
    size_t li = 0;
    while (lhs[li] == ' ' || lhs[li] == '\t') li++;
    if (li) memmove(lhs, lhs + li, ln - li + 1);

    size_t rn = strlen(rhs);
    while (rn > 0 && (rhs[rn - 1] == ' ' || rhs[rn - 1] == '\t')) rhs[--rn] = '\0';
    size_t ri = 0;
    while (rhs[ri] == ' ' || rhs[ri] == '\t') ri++;
    if (ri) memmove(rhs, rhs + ri, rn - ri + 1);

    /* Determine operator string */
    if (op_len == 2) {
        if (op[0] == '=') strcpy(op_str, "=");
        else if (op[0] == '!') strcpy(op_str, "!=");
        else if (op[0] == '<') strcpy(op_str, "<=");
        else if (op[0] == '>') strcpy(op_str, ">=");
    } else {
        op_str[0] = op[0];
        op_str[1] = '\0';
    }

    if (strcmp(op_str, "!=") == 0) {
        snprintf(out, cap, "(not (= %s %s))", lhs, rhs);
    } else {
        snprintf(out, cap, "(%s %s %s)", op_str, lhs, rhs);
    }
}

static bool dap_on_cond(TuriEnv *env, const char *condition, void *ud) {
    (void)ud;
    char rewritten[512];
    dap_rewrite_condition(condition, rewritten, sizeof rewritten);
    char val[256];
    if (!turi_debug_eval_expr(env, 0, rewritten, val, sizeof val)) {
        return true; /* evaluation failed -- fall back to stop */
    }
    /* Stop if the evaluated value is truthy (not false and not nil) */
    return (strcmp(val, "false") != 0 && strcmp(val, "nil") != 0);
}

/* ---------------------------------------------------------------------------
 * Breakpoints
 * --------------------------------------------------------------------------- */

/* Apply a setBreakpoints request for one source.  Pre-launch (env NULL) the
 * lines are staged in DapState; while paused they are applied live.  Either way
 * the previous breakpoints for this source's basename are cleared first
 * (setBreakpoints is a full replacement per source). */
static void dap_set_breakpoints(DapState *s, int64_t req_seq, const char *args) {
    char path[1024] = {0};
    size_t srclen;
    const char *src = lsp_json_raw(args, "source", &srclen);
    if (src) lsp_json_str_copy(src, "path", path, sizeof path);
    const char *base = dap_basename(path);

    if (s->env) {
        turi_debug_clear_breakpoints_for_file(s->env, base);
    } else {
        int w = 0;
        for (int i = 0; i < s->n_bps; i++)
            if (strcmp(s->bps[i].file, base) != 0) {
                if (w != i) s->bps[w] = s->bps[i];
                w++;
            }
        s->n_bps = w;
    }

    Buf body; buf_init(&body);
    buf_puts(&body, "{\"breakpoints\":[");

    size_t bplen;
    const char *bparr = lsp_json_raw(args, "breakpoints", &bplen);
    int emitted = 0;
    if (bparr) {
        const char *cur = bparr;
        for (;;) {
            const char *obj = dap_array_next_obj(&cur);
            if (!obj) break;
            int64_t line = lsp_json_int(obj, "line");
            if (line <= 0) continue;
            char cond[160] = {0};
            lsp_json_str_copy(obj, "condition", cond, sizeof cond);

            bool ok = true;
            if (s->env) {
                ok = turi_debug_add_breakpoint(s->env, base, (uint32_t)line, cond) > 0;
            } else if (s->n_bps < DAP_MAX_BP) {
                DapBp *bp = &s->bps[s->n_bps++];
                memset(bp, 0, sizeof *bp);
                snprintf(bp->file, sizeof bp->file, "%s", base);
                bp->line = (uint32_t)line;
                snprintf(bp->cond, sizeof bp->cond, "%s", cond);
            } else {
                ok = false;
            }
            if (emitted++) buf_putc(&body, ',');
            buf_printf(&body, "{\"verified\":%s,\"line\":%lld}",
                       ok ? "true" : "false", (long long)line);
        }
    }
    buf_puts(&body, "]}");
    dap_send_response(s, req_seq, "setBreakpoints", dap_cstr(&body), true);
    buf_free(&body);
}

/* ---------------------------------------------------------------------------
 * Stack / scopes / variables
 * --------------------------------------------------------------------------- */

/* One frame, from whichever source the session is being served from.
 *
 * The replay's TurTraceFrame and the live TuriDbgFrame carry the same four
 * things in the same order (innermost first), so the two paths differ by where
 * the fields come from and by nothing else -- which is the point of the
 * replay: a client cannot tell a recorded session from a live one except by
 * what it is allowed to ask. */
static bool dap_frame_at(DapState *s, int idx, const char **fn,
                         const char **file, uint32_t *line, uint32_t *col) {
    if (s->replay_mode) {
        TurTraceFrame tf;
        if (!turi_trace_replay_frame_at(s->replay, idx, &tf)) return false;
        *fn = tf.fn_name; *file = tf.file_path;
        *line = tf.line;  *col = tf.col;
        return true;
    }
    if (!s->env) return false;
    static TuriDbgFrame fr;
    if (!turi_debug_frame_at(s->env, idx, &fr)) return false;
    *fn = fr.fn_name; *file = fr.file_path;
    *line = fr.line;  *col = fr.col;
    return true;
}

static int dap_frame_count(DapState *s) {
    if (s->replay_mode) return turi_trace_replay_frame_count(s->replay);
    return s->env ? turi_debug_frame_count(s->env) : 0;
}

static void dap_stack_trace(DapState *s, int64_t req_seq) {
    Buf body; buf_init(&body);
    buf_puts(&body, "{\"stackFrames\":[");
    int n = dap_frame_count(s);
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        const char *fn = "", *file = "";
        uint32_t fline = 0, fcol = 0;
        if (!dap_frame_at(s, i, &fn, &file, &fline, &fcol)) continue;
        struct { const char *fn_name; const char *file_path;
                 uint32_t line, col; } fr = { fn, file, fline, fcol };
        if (emitted++) buf_putc(&body, ',');
        buf_printf(&body, "{\"id\":%d,\"name\":\"", i);
        dap_json_escape(&body, fr.fn_name);
        buf_printf(&body, "\",\"line\":%u,\"column\":%u",
                   fr.line, fr.col);
        if (fr.file_path[0]) {
            buf_puts(&body, ",\"source\":{\"name\":\"");
            dap_json_escape(&body, dap_basename(fr.file_path));
            buf_puts(&body, "\",\"path\":\"");
            dap_json_escape(&body, fr.file_path);
            buf_puts(&body, "\"}");
        }
        buf_putc(&body, '}');
    }
    buf_printf(&body, "],\"totalFrames\":%d}", emitted);
    dap_send_response(s, req_seq, "stackTrace", dap_cstr(&body), true);
    buf_free(&body);
}

static void dap_scopes(DapState *s, int64_t req_seq, const char *args) {
    int64_t frame_id = lsp_json_int(args, "frameId");
    if (frame_id < 0) frame_id = 0;
    char body[256];
    /* variablesReference = frameId + 1 so a reference never collides with 0
     * (the DAP "no children" sentinel). */
    snprintf(body, sizeof body,
        "{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":%lld,"
        "\"expensive\":false}]}", (long long)(frame_id + 1));
    dap_send_response(s, req_seq, "scopes", body, true);
}

typedef struct { Buf *b; int n; } VarCollect;

static void dap_var_cb(const char *name, const char *repr, void *ud) {
    VarCollect *vc = (VarCollect *)ud;
    if (vc->n++) buf_putc(vc->b, ',');
    buf_puts(vc->b, "{\"name\":\"");
    dap_json_escape(vc->b, name);
    buf_puts(vc->b, "\",\"value\":\"");
    dap_json_escape(vc->b, repr);
    buf_puts(vc->b, "\",\"variablesReference\":0}");
}

static void dap_variables(DapState *s, int64_t req_seq, const char *args) {
    int64_t ref = lsp_json_int(args, "variablesReference");
    int frame = (ref > 0) ? (int)(ref - 1) : 0;
    Buf body; buf_init(&body);
    buf_puts(&body, "{\"variables\":[");
    VarCollect vc = { &body, 0 };
    if (s->replay_mode) {
        int n = turi_trace_replay_local_count(s->replay, frame);
        for (int i = 0; i < n; i++) {
            const char *name = "", *repr = "";
            if (turi_trace_replay_local_at(s->replay, frame, i, &name, &repr))
                dap_var_cb(name, repr, &vc);
        }
    } else if (s->env) {
        turi_debug_frame_locals(s->env, frame, dap_var_cb, &vc);
    }
    buf_puts(&body, "]}");
    dap_send_response(s, req_seq, "variables", dap_cstr(&body), true);
    buf_free(&body);
}

static void dap_evaluate(DapState *s, int64_t req_seq, const char *args) {
    if (s->replay_mode) {
        /* The one request a recording cannot answer: there is no live frame to
         * evaluate in. Saying so beats returning a stale value that looks like
         * an answer. */
        dap_send_error(s, req_seq, "evaluate",
                       "cannot evaluate in a recording -- "
                       "relaunch without \"replay\" for a live session");
        return;
    }
    char expr[256] = {0};
    lsp_json_str_copy(args, "expression", expr, sizeof expr);
    int64_t frame_id = lsp_json_int(args, "frameId");
    if (frame_id < 0) frame_id = 0;
    char val[512];
    if (s->env && turi_debug_eval_expr(s->env, (int)frame_id, expr, val, sizeof val)) {
        Buf body; buf_init(&body);
        buf_puts(&body, "{\"result\":\"");
        dap_json_escape(&body, val);
        buf_puts(&body, "\",\"variablesReference\":0}");
        dap_send_response(s, req_seq, "evaluate", dap_cstr(&body), true);
        buf_free(&body);
    } else {
        dap_send_error(s, req_seq, "evaluate", val[0] ? val : "not in scope");
    }
}

/* ---------------------------------------------------------------------------
 * T4-T6: the timeline extension (`replayInfo` / `replaySeek` / `replaySites`)
 *
 * DAP describes execution as a sequence of steps, never as an axis. That is
 * the right model for a live debuggee -- there is nowhere to scrub to -- but a
 * recording IS an axis, and the three things a scrubber needs of one are its
 * length, a way to jump to an arbitrary point, and a shape to draw. None has a
 * standard request, and approximating them costs more than it looks:
 *
 *  - A slider with no length has no range, and a range that is a guess is
 *    worse than no slider.
 *  - Approximating a seek with repeated `stepBack` is the trap trace.h already
 *    documents: every seek rebuilds state from the start of the stream, so
 *    doing it once per candidate turns a scan of an 80k recording from
 *    milliseconds into a hang.
 *  - A depth ribbon samples one value per pixel across the whole run, which
 *    would be that trap at its very worst -- and `turi_trace_replay_depth_at`
 *    exists precisely so it does not have to be.
 *
 * So these are three custom requests over the reader that already answers all
 * of it. Custom, not proposed-standard: they are meaningful only for a session
 * served from a recording, and a client that does not know them is not missing
 * anything it could have used. A client detects them from
 * `supportsTurmericReplayTimeline` in the initialize response.
 * --------------------------------------------------------------------------- */

/* How many buckets a depth ribbon gets when the client does not say. Chosen as
 * roughly the pixel width a ribbon is drawn at; the point of downsampling here
 * rather than in the client is that the client would otherwise have to ask for
 * every step to do it. */
#define DAP_SITES_DEFAULT_BUCKETS 256
#define DAP_SITES_MAX_BUCKETS     4096
/* An explicit index list is a client asking about specific steps -- a cursor
 * readout, a tooltip -- not a scan. Bounded so a malformed request cannot make
 * the adapter build an unbounded response. */
#define DAP_SITES_MAX_INDICES     4096

/* One site entry: where step `index` was, and how deep the stack was there. */
static void dap_write_site(DapState *s, Buf *b, uint32_t index, int depth,
                           bool first) {
    const char *file = "";
    uint32_t line = 0;
    turi_trace_replay_site_at(s->replay, index, &file, &line);
    buf_printf(b, "%s{\"index\":%u,\"line\":%u,\"depth\":%d,\"file\":\"",
               first ? "" : ",", index, line, depth);
    dap_json_escape(b, file);
    buf_puts(b, "\"}");
}

/* Parse a JSON array of non-negative integers. Returns the count written. */
static int dap_parse_int_array(const char *json, const char *key,
                               uint32_t *out, int cap) {
    size_t l;
    const char *arr = json ? lsp_json_raw(json, key, &l) : NULL;
    if (!arr) return -1;   /* absent, which is different from empty */
    int n = 0;
    for (const char *p = arr; *p && *p != ']'; p++) {
        if (*p < '0' || *p > '9') continue;
        long long v = atoll(p);
        if (n < cap) out[n++] = (uint32_t)(v < 0 ? 0 : v);
        while (p[1] >= '0' && p[1] <= '9') p++;
    }
    return n;
}

static void dap_replay_info(DapState *s, int64_t req_seq) {
    uint32_t steps = turi_trace_replay_steps(s->replay);
    uint32_t index = turi_trace_replay_index(s->replay);
    Buf b; buf_init(&b);
    buf_printf(&b, "{\"steps\":%u,\"index\":%u,\"depth\":%d,\"outputLength\":%zu}",
               steps, index, turi_trace_replay_depth_at(s->replay, index),
               s->replay_out_sent);
    dap_send_response(s, req_seq, "replayInfo", dap_cstr(&b), true);
    buf_free(&b);
}

/* Where steps are and how deep they are -- `{index, file, line, depth}` each.
 *
 * The shape is Try Turmeric's `trace-site-at`
 * (`web/public/eval-worker.js`, `turi_wasm_trace_site_at` in
 * `src/web/wasm_glue.c`), which returns position and depth **together**,
 * batched over many indices in one round trip. That matters because the two
 * callers want the same data: a timeline's cursor readout needs `file:line`,
 * a depth ribbon needs `depth`, and asking for them separately doubles the
 * traffic for no reason. An earlier draft of this served only a depths array
 * and would have forced exactly that.
 *
 * Two ways to ask, because the two callers scan differently:
 *
 *  - `{"indices": [...]}` -- specific steps. A cursor readout, a tooltip.
 *  - `{"buckets": N}`     -- the whole recording downsampled to N samples.
 *
 * A bucket reports the MAXIMUM depth in its range and the site of the step
 * where that maximum occurred, not the bucket's first step. A ribbon is read
 * for recursion shape, and a deep call falling between two samples is exactly
 * what the reader is looking for -- sampling or averaging would quietly erase
 * it, and reporting the deepest step's position means clicking a spike goes
 * where the spike is.
 *
 * Neither form seeks. `depth_at` and `site_at` are index reads by
 * construction (see trace.h), which is what keeps a full-width ribbon over a
 * 1M-step recording a scan rather than a hang. */
static void dap_replay_sites(DapState *s, int64_t req_seq, const char *args) {
    uint32_t steps = turi_trace_replay_steps(s->replay);
    Buf b; buf_init(&b);
    buf_printf(&b, "{\"steps\":%u,\"sites\":[", steps);

    uint32_t idx[DAP_SITES_MAX_INDICES];
    int n_idx = dap_parse_int_array(args, "indices", idx, DAP_SITES_MAX_INDICES);
    if (n_idx >= 0) {
        /* Explicit lookups. An out-of-range index answers with depth 0 and an
         * empty file rather than failing the whole batch: one bad index in a
         * tooltip request should not cost the client the other forty. */
        for (int i = 0; i < n_idx; i++) {
            dap_write_site(s, &b, idx[i],
                           turi_trace_replay_depth_at(s->replay, idx[i]), i == 0);
        }
    } else {
        int64_t want = args ? lsp_json_int(args, "buckets") : -1;
        uint32_t buckets = (want > 0) ? (uint32_t)want : DAP_SITES_DEFAULT_BUCKETS;
        if (buckets > DAP_SITES_MAX_BUCKETS) buckets = DAP_SITES_MAX_BUCKETS;
        /* Never more buckets than steps: empty buckets at the tail would draw
         * as a ribbon that falls to zero before the recording ends. */
        if (buckets > steps) buckets = steps;
        for (uint32_t i = 0; i < buckets; i++) {
            /* 64-bit intermediate: steps * buckets overflows 32 bits at the
             * 1M step cap with a wide ribbon. */
            uint32_t lo = (uint32_t)(((uint64_t)i * steps) / buckets);
            uint32_t hi = (uint32_t)(((uint64_t)(i + 1) * steps) / buckets);
            if (hi <= lo) hi = lo + 1;
            int peak = 0;
            uint32_t peak_at = lo;
            for (uint32_t j = lo; j < hi && j < steps; j++) {
                int d = turi_trace_replay_depth_at(s->replay, j);
                if (d > peak) { peak = d; peak_at = j; }
            }
            dap_write_site(s, &b, peak_at, peak, i == 0);
        }
    }
    buf_puts(&b, "]}");
    dap_send_response(s, req_seq, "replaySites", dap_cstr(&b), true);
    buf_free(&b);
}

/* Handle a request that is valid both pre-launch and while paused (the
 * introspection + breakpoint surface).  Returns true if `cmd` was handled. */
static bool dap_handle_common(DapState *s, const char *cmd, int64_t req_seq,
                              const char *args) {
    /* The timeline extension. Checked first and answered in both loops, so a
     * live session gets the reason rather than the generic "not supported
     * while paused" -- a client that asked has a recording in mind, and
     * "relaunch with replay" is the actionable answer. */
    if (!strcmp(cmd, "replayInfo") || !strcmp(cmd, "replaySites") ||
        !strcmp(cmd, "replaySeek")) {
        if (!s->replay_mode || !s->replay) {
            dap_send_error(s, req_seq, cmd,
                           "there is no recording in this session -- "
                           "relaunch with \"replay\": true");
            return true;
        }
        if (!strcmp(cmd, "replayInfo"))  { dap_replay_info(s, req_seq);        return true; }
        if (!strcmp(cmd, "replaySites")) { dap_replay_sites(s, req_seq, args); return true; }
        /* `replaySeek` moves the cursor, so it is handled by the replay
         * session loop, which owns that. Reaching here means it was sent from
         * somewhere that cannot move -- pre-launch, before the recording
         * exists. */
        dap_send_error(s, req_seq, cmd, "no cursor to seek yet; wait for the first stop");
        return true;
    }
    if (!strcmp(cmd, "setBreakpoints"))      { dap_set_breakpoints(s, req_seq, args); return true; }
    if (!strcmp(cmd, "setExceptionBreakpoints")) {
        dap_send_response(s, req_seq, "setExceptionBreakpoints", "{\"breakpoints\":[]}", true);
        return true;
    }
    if (!strcmp(cmd, "setFunctionBreakpoints")) {
        dap_send_response(s, req_seq, "setFunctionBreakpoints", "{\"breakpoints\":[]}", true);
        return true;
    }
    if (!strcmp(cmd, "threads")) {
        dap_send_response(s, req_seq, "threads",
                          "{\"threads\":[{\"id\":1,\"name\":\"main\"}]}", true);
        return true;
    }
    if (!strcmp(cmd, "stackTrace")) { dap_stack_trace(s, req_seq);        return true; }
    if (!strcmp(cmd, "scopes"))     { dap_scopes(s, req_seq, args);       return true; }
    if (!strcmp(cmd, "variables"))  { dap_variables(s, req_seq, args);    return true; }
    if (!strcmp(cmd, "evaluate"))   { dap_evaluate(s, req_seq, args);     return true; }
    if (!strcmp(cmd, "source")) {
        dap_send_error(s, req_seq, "source", "source bytes not served; open the file directly");
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Pause handler (invoked from inside the eval loop while suspended)
 * --------------------------------------------------------------------------- */

static void dap_on_pause(TuriEnv *env, TuriDbgStop reason, void *ud) {
    DapState *s = (DapState *)ud;

    /* The program-entry stop is swallowed unless the client asked for it. */
    if (reason == TURI_DBG_STOP_ENTRY && !s->stop_on_entry) {
        turi_debug_resume_continue(env);
        return;
    }

    dap_drain_output(s);

    const char *rs =
        reason == TURI_DBG_STOP_BREAKPOINT ? "breakpoint" :
        reason == TURI_DBG_STOP_ENTRY      ? "entry"      :
        reason == TURI_DBG_STOP_PAUSE      ? "pause"      : "step";
    char ev[160];
    snprintf(ev, sizeof ev,
        "{\"reason\":\"%s\",\"threadId\":1,\"allThreadsStopped\":true}", rs);
    dap_send_event(s, "stopped", ev);

    for (;;) {
        char *msg = lsp_read_message(s->in_fd);
        if (!msg) { turi_debug_resume_continue(env); return; }  /* EOF: run out */

        char cmd[64];
        if (lsp_json_str_copy(msg, "command", cmd, sizeof cmd) != 0) { free(msg); continue; }
        int64_t rq = lsp_json_int(msg, "seq");
        size_t al;
        const char *ar = lsp_json_raw(msg, "arguments", &al);
        char *args = NULL;
        if (ar) { args = (char *)malloc(al + 1); memcpy(args, ar, al); args[al] = '\0'; }

        bool resume = false;
        if (!strcmp(cmd, "continue")) {
            turi_debug_resume_continue(env);
            dap_send_response(s, rq, "continue", "{\"allThreadsContinued\":true}", true);
            resume = true;
        } else if (!strcmp(cmd, "next")) {
            turi_debug_resume_step_over(env);
            dap_send_response(s, rq, "next", NULL, true);
            resume = true;
        } else if (!strcmp(cmd, "stepIn")) {
            turi_debug_resume_step_in(env);
            dap_send_response(s, rq, "stepIn", NULL, true);
            resume = true;
        } else if (!strcmp(cmd, "stepOut")) {
            turi_debug_resume_step_out(env);
            dap_send_response(s, rq, "stepOut", NULL, true);
            resume = true;
        } else if (!strcmp(cmd, "pause")) {
            dap_send_response(s, rq, "pause", NULL, true);  /* already paused */
        } else if (!strcmp(cmd, "disconnect") || !strcmp(cmd, "terminate")) {
            dap_send_response(s, rq, cmd, NULL, true);
            free(args); free(msg);
            _exit(0);
        } else if (!dap_handle_common(s, cmd, rq, args)) {
            dap_send_error(s, rq, cmd, "not supported while paused");
        }
        free(args);
        free(msg);
        if (resume) return;
    }
}

/* ---------------------------------------------------------------------------
 * T2: reverse execution over a recording
 *
 * VS Code and nvim-dap already draw the whole reverse-execution UI when a
 * server advertises supportsStepBack -- the scrubber, the backwards
 * breakpoints, the rewinding variables pane. So the leverage here is entirely
 * in answering the requests: there is no widget to write.
 *
 * The session model is the plain one turned inside out. A live session pauses
 * the program and asks it questions; this one runs the program to completion
 * with the recorder attached and then asks the recording. Everything except
 * `evaluate` answers identically, because a frame and its locals are exactly
 * what the recording stores.
 * --------------------------------------------------------------------------- */

/* Every OUTPUT record in the recording, regardless of the cursor.
 *
 * Needed because `turi_trace_replay_output` reports what was printed strictly
 * BEFORE the cursor's step, and a program whose final act is a `println`
 * drains it after the final STEP -- so the last step's transcript is missing
 * the last thing the program printed. Measured: the replay fixture reports
 * outputLength 0 at step 24020 of 24021 when its only print is trailing.
 *
 * An empty console at the end of a run that printed reads as a broken timeline
 * rather than a precise one. Try Turmeric hit this first and answered it the
 * same way -- `turi_wasm_trace_output_full` in src/web/wasm_glue.c, asked for
 * only at the last step -- and this is that function for the DAP side. */
static void dap_replay_output_full(DapState *s, Buf *out) {
    if (!s->trace) return;
    size_t len = 0;
    const uint8_t *bytes = turi_trace_bytes(s->trace, &len);
    TurTraceReader r;
    if (!bytes || !turi_trace_open(&r, bytes, len)) return;
    TurTraceRecord rec;
    while (turi_trace_next(&r, &rec)) {
        if (rec.tag != TUR_TRACE_OUTPUT || !rec.payload) continue;
        buf_write(out, (const char *)rec.payload, rec.payload_len);
    }
}

/* Is the cursor on the recording's final step? Only there does the transcript
 * need the whole-recording treatment above; everywhere else the cursor-relative
 * answer is the correct one, and is what makes scrubbing show the program's
 * output as it accumulated. */
static bool dap_replay_at_last_step(DapState *s) {
    uint32_t n = turi_trace_replay_steps(s->replay);
    return n > 0 && turi_trace_replay_index(s->replay) == n - 1;
}

/* Send the recorded output the cursor has now passed.
 *
 * Forward motion appends, as a standard `output` event: that is what every DAP
 * client already understands, and a terminal has no undo.
 *
 * Backward motion is the case a plain `output` event cannot express. The
 * transcript at the new cursor is a PREFIX of what was already sent, and the
 * client has no way to work out where to cut -- it has only ever been told
 * deltas. Silently sending nothing (what this did before) leaves the console
 * showing output from steps the cursor has since rewound past, which is the
 * one thing a time-travel console must not do.
 *
 * So a shrink emits `replayOutput` carrying the whole transcript, to be used
 * in place of what the client has. A client that does not know the event
 * ignores it and is no worse off than before; one that does can mirror the
 * console exactly. Whole-transcript rather than a truncation offset because a
 * client that missed an earlier event would otherwise cut to the wrong place
 * and have no way to notice. */
static void dap_replay_flush_output(DapState *s) {
    /* At the last step the transcript is the whole recording's, not the
     * cursor's -- see dap_replay_output_full. `full` owns those bytes when it
     * is used; `out` points into the replay otherwise. */
    Buf full; buf_init(&full);
    const char *out = NULL;
    size_t len = 0;
    if (dap_replay_at_last_step(s)) {
        dap_replay_output_full(s, &full);
        out = full.data ? full.data : "";
        len = full.len;
    } else {
        out = turi_trace_replay_output(s->replay, &len);
    }
    if (len == s->replay_out_sent) { buf_free(&full); return; }

    Buf b; buf_init(&b);
    if (len < s->replay_out_sent) {
        buf_printf(&b, "{\"seq\":%d,\"type\":\"event\",\"event\":\"replayOutput\","
                       "\"body\":{\"category\":\"stdout\",\"length\":%zu,"
                       "\"output\":\"", s->seq++, len);
        dap_json_escape_n(&b, out, len);
    } else {
        buf_printf(&b, "{\"seq\":%d,\"type\":\"event\",\"event\":\"output\","
                       "\"body\":{\"category\":\"stdout\",\"output\":\"", s->seq++);
        dap_json_escape_n(&b, out + s->replay_out_sent, len - s->replay_out_sent);
    }
    buf_puts(&b, "\"}}");
    dap_write(s, &b);
    buf_free(&b);
    buf_free(&full);
    s->replay_out_sent = len;
}

/* Is `index` on a line the client set a breakpoint on?
 *
 * Asked of every step between the cursor and the next hit, so it reads the
 * step's recorded site directly rather than seeking to it: a seek rebuilds the
 * whole state from the start of the stream, and doing that per candidate turns
 * a `continue` over an 80k-step recording from instant into a hang. Measured.
 *
 * Conditions are not evaluated here. A condition is an expression in a frame,
 * and a recording has no frame to evaluate it in -- the same reason `evaluate`
 * refuses. A conditional breakpoint therefore behaves as an unconditional one
 * in replay, which is the honest degradation: it stops more often than asked,
 * never less. */
static bool dap_replay_is_bp(DapState *s, uint32_t index) {
    const char *file = "";
    uint32_t line = 0;
    if (!turi_trace_replay_site_at(s->replay, index, &file, &line)) return false;
    for (int i = 0; i < s->n_bps; i++) {
        if (s->bps[i].line != line) continue;
        if (s->bps[i].file[0] &&
            strcmp(s->bps[i].file, dap_basename(file)) != 0)
            continue;
        return true;
    }
    return false;
}

/* Scan for the next (dir > 0) or previous breakpoint hit; falls back to the
 * end of the recording, which is what a `continue` with nothing ahead of it
 * should do. */
static uint32_t dap_replay_seek_bp(DapState *s, int dir, bool *hit_out) {
    uint32_t n = turi_trace_replay_steps(s->replay);
    uint32_t cur = turi_trace_replay_index(s->replay);
    if (hit_out) *hit_out = false;
    if (n == 0) return 0;
    if (dir > 0) {
        for (uint32_t i = cur + 1; i < n; i++) {
            if (!dap_replay_is_bp(s, i)) continue;
            if (hit_out) *hit_out = true;
            return i;
        }
        return n - 1;
    }
    for (uint32_t i = cur; i > 0; i--) {
        if (!dap_replay_is_bp(s, i - 1)) continue;
        if (hit_out) *hit_out = true;
        return i - 1;
    }
    return 0;
}

/* Would landing on step `i` read as having moved, from a step at
 * (from_file, from_line, from_depth)?  See dap_replay_seek_line. */
static bool replay_step_is_move(DapState *s, uint32_t i, const char *from_file,
                                uint32_t from_line, int from_depth,
                                int max_depth) {
    int depth = turi_trace_replay_depth_at(s->replay, i);
    if (max_depth >= 0 && depth > max_depth) return false;
    if (depth != from_depth) return true;
    const char *file = NULL;
    uint32_t    line = 0;
    turi_trace_replay_site_at(s->replay, i, &file, &line);
    if (line != from_line) return true;
    if (file && from_file) return strcmp(file, from_file) != 0;
    return file != from_file;
}

/* Advance (or rewind) to the next step a DAP client would call a step.
 *
 * A recording is taken per expression, but DAP is a line protocol: an editor
 * draws a line marker, so `stepIn` and `next` have to land somewhere the
 * marker visibly moves.  Mapping them onto raw trace indices would step
 * through the sub-expressions of one line and look, four keypresses running,
 * like a debugger that has stopped responding.
 *
 * So the recording stays fine and the presentation is coarse: stop at the
 * first step whose source line differs -- or whose depth differs, which is how
 * a call to a one-line function on the current line still registers as
 * entering something rather than being skipped over.
 *
 * `max_depth` >= 0 additionally requires the landing step to be at that depth
 * or shallower, which is what makes this step-over; pass -1 for step-in. */
static uint32_t dap_replay_seek_line(DapState *s, int dir, int max_depth) {
    uint32_t n = turi_trace_replay_steps(s->replay);
    uint32_t cur = turi_trace_replay_index(s->replay);
    if (n == 0) return 0;

    const char *from_file = NULL;
    uint32_t    from_line = 0;
    turi_trace_replay_site_at(s->replay, cur, &from_file, &from_line);
    int from_depth = turi_trace_replay_depth_at(s->replay, cur);

    /* Written as two loops rather than one with a signed cursor: `i` is a
     * uint32_t and the backwards scan runs down to and including index 0. */
    if (dir > 0) {
        for (uint32_t i = cur + 1; i < n; i++)
            if (replay_step_is_move(s, i, from_file, from_line, from_depth,
                                    max_depth))
                return i;
        return n - 1;
    }
    for (uint32_t i = cur; i > 0; i--)
        if (replay_step_is_move(s, i - 1, from_file, from_line, from_depth,
                                max_depth))
            return i - 1;
    return 0;
}

/* Advance (or rewind) until the frame depth comes back to `want` or shallower
 * -- which is step-out when `want` is one less than the current depth. */
static uint32_t dap_replay_seek_depth(DapState *s, int dir, int want) {
    uint32_t n = turi_trace_replay_steps(s->replay);
    uint32_t cur = turi_trace_replay_index(s->replay);
    if (n == 0) return 0;
    if (dir > 0) {
        for (uint32_t i = cur + 1; i < n; i++)
            if (turi_trace_replay_depth_at(s->replay, i) <= want) return i;
        return n - 1;
    }
    for (uint32_t i = cur; i > 0; i--)
        if (turi_trace_replay_depth_at(s->replay, i - 1) <= want) return i - 1;
    return 0;
}

/* The replay session loop. Returns when the client disconnects, or when a
 * forward `continue` reaches the end of the recording -- at which point
 * dap_run_program emits `exited` / `terminated` exactly as it does for a live
 * run. */
static void dap_replay_session(DapState *s) {
    uint32_t n = turi_trace_replay_steps(s->replay);
    if (n == 0) return;

    turi_trace_replay_seek(s->replay, 0);
    const char *reason = "entry";
    if (!s->stop_on_entry) {
        bool hit = false;
        uint32_t to = dap_replay_seek_bp(s, +1, &hit);
        if (!hit) return;   /* nothing to stop at: the run is simply over */
        turi_trace_replay_seek(s->replay, to);
        reason = "breakpoint";
    }

    for (;;) {
        dap_replay_flush_output(s);
        char ev[160];
        snprintf(ev, sizeof ev,
            "{\"reason\":\"%s\",\"threadId\":1,\"allThreadsStopped\":true}",
            reason);
        dap_send_event(s, "stopped", ev);
        reason = "step";

        bool moved = false;
        while (!moved) {
            char *msg = lsp_read_message(s->in_fd);
            if (!msg) return;   /* EOF: the client is gone */

            char cmd[64];
            if (lsp_json_str_copy(msg, "command", cmd, sizeof cmd) != 0) {
                free(msg);
                continue;
            }
            int64_t rq = lsp_json_int(msg, "seq");
            size_t alen;
            const char *araw = lsp_json_raw(msg, "arguments", &alen);
            char *args = NULL;
            if (araw) {
                args = (char *)malloc(alen + 1);
                memcpy(args, araw, alen);
                args[alen] = '\0';
            }

            uint32_t cur   = turi_trace_replay_index(s->replay);
            int      depth = turi_trace_replay_depth_at(s->replay, cur);
            uint32_t to    = cur;
            bool     stop  = false;   /* leave the loop entirely */

            if (!strcmp(cmd, "continue")) {
                bool hit = false;
                to = dap_replay_seek_bp(s, +1, &hit);
                if (!hit) stop = true;   /* ran off the end */
                else reason = "breakpoint";
                dap_send_response(s, rq, "continue",
                                  "{\"allThreadsContinued\":true}", true);
                moved = true;
            } else if (!strcmp(cmd, "reverseContinue")) {
                bool hit = false;
                to = dap_replay_seek_bp(s, -1, &hit);
                if (hit) reason = "breakpoint";
                dap_send_response(s, rq, "reverseContinue", NULL, true);
                moved = true;
            } else if (!strcmp(cmd, "stepIn")) {
                to = dap_replay_seek_line(s, +1, -1);
                if (to == cur) stop = true;
                dap_send_response(s, rq, "stepIn", NULL, true);
                moved = true;
            } else if (!strcmp(cmd, "stepBack")) {
                to = dap_replay_seek_line(s, -1, -1);
                dap_send_response(s, rq, "stepBack", NULL, true);
                moved = true;
            } else if (!strcmp(cmd, "next")) {
                to = dap_replay_seek_line(s, +1, depth);
                if (to == cur) stop = true;
                dap_send_response(s, rq, "next", NULL, true);
                moved = true;
            } else if (!strcmp(cmd, "reverseNext")) {
                to = dap_replay_seek_line(s, -1, depth);
                dap_send_response(s, rq, "reverseNext", NULL, true);
                moved = true;
            } else if (!strcmp(cmd, "stepOut")) {
                to = dap_replay_seek_depth(s, +1, depth - 1);
                if (to == cur) stop = true;
                dap_send_response(s, rq, "stepOut", NULL, true);
                moved = true;
            } else if (!strcmp(cmd, "replaySeek")) {
                /* The one request that moves the cursor to somewhere neither
                 * stepping nor breakpoints could reach: an arbitrary index.
                 * This is what makes a slider a slider.
                 *
                 * A missing or negative `index` clamps to 0 rather than
                 * erroring -- lsp_json_int reports both as -1, and a scrubber
                 * dragged to the far left means the start. The reader clamps
                 * the upper end itself and reports where it actually landed,
                 * which is the value the client should believe over its own
                 * arithmetic. */
                int64_t want = args ? lsp_json_int(args, "index") : 0;
                if (want < 0) want = 0;
                uint32_t n_steps = turi_trace_replay_steps(s->replay);
                to = (want >= (int64_t)n_steps && n_steps > 0)
                       ? n_steps - 1 : (uint32_t)want;
                char body[64];
                snprintf(body, sizeof body, "{\"index\":%u}", to);
                dap_send_response(s, rq, "replaySeek", body, true);
                /* Reported as a `step` stop, because that is what it is from
                 * the client's side: the cursor moved, and every pane that
                 * follows the cursor has to refresh. */
                moved = true;
            } else if (!strcmp(cmd, "pause")) {
                dap_send_response(s, rq, "pause", NULL, true);
            } else if (!strcmp(cmd, "disconnect") || !strcmp(cmd, "terminate")) {
                dap_send_response(s, rq, cmd, NULL, true);
                free(args); free(msg);
                return;
            } else if (!dap_handle_common(s, cmd, rq, args)) {
                dap_send_error(s, rq, cmd, "not supported in a recording");
            }

            free(args);
            free(msg);
            if (moved) {
                turi_trace_replay_seek(s->replay, to);
                if (stop) { dap_replay_flush_output(s); return; }
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public entry points
 * --------------------------------------------------------------------------- */

void dap_begin_session(void *state, TuriEnv *env) {
    DapState *s = (DapState *)state;
    s->env = env;
    if (s->replay_mode) {
        /* The recorder owns the pause handler for the whole run -- it stops at
         * every node and immediately resumes, so breakpoints have nothing to
         * do until the replay, where they are matched against recorded sites
         * instead. */
        TurTraceOpts opts;
        memset(&opts, 0, sizeof opts);
        opts.capture_output = true;
        s->trace = turi_trace_begin(env, &opts);
        if (s->trace) return;
        /* No recorder: fall through to a live session rather than running the
         * program with no debugger attached at all. */
        s->replay_mode = false;
    }
    turi_debug_set_pause_handler(env, dap_on_pause, s);
    turi_debug_set_cond_handler(env, dap_on_cond, s);
    for (int i = 0; i < s->n_bps; i++)
        turi_debug_add_breakpoint(env, s->bps[i].file, s->bps[i].line, s->bps[i].cond);
}

void dap_end_session(void *state, TuriEnv *env) {
    (void)env;
    DapState *s = (DapState *)state;
    /* The recorder's pause handler lives on the env, so it has to come off
     * while the env is still there. The recording itself outlives it -- that
     * is the whole point. */
    if (s->trace) turi_trace_stop(s->trace);
    /* And the env itself is about to be freed. A replay session keeps serving
     * requests after this point -- that is what replay IS -- so anything that
     * still reached for `s->env` would be reading freed memory. Every user of
     * it already guards on NULL; this is what makes the guard true. */
    s->env = NULL;
}

/* Run the program with the debuggee's stdout captured to a pipe so it does not
 * corrupt the JSON-RPC channel; forward it as `output` events. */
static void dap_run_program(DapState *s, DapLaunchFn launch, void *ud,
                            const char *program, char **args, int n_args) {
    int saved = -1;
    s->out_pipe_r = -1;
#ifndef _WIN32
    int p[2];
    if (pipe(p) == 0) {
        fcntl(p[0], F_SETFL, O_NONBLOCK);
#ifdef F_SETPIPE_SZ
        fcntl(p[1], F_SETPIPE_SZ, 1 << 20);  /* best-effort: 1 MiB buffer */
#endif
        s->out_pipe_r = p[0];
        fflush(stdout);
        saved = dup(STDOUT_FILENO);
        dup2(p[1], STDOUT_FILENO);
        close(p[1]);
    }
#else
    /*
     * Not captured on Windows.  _pipe/_dup2 exist, but the non-blocking read
     * this relies on does not: fcntl/O_NONBLOCK have no counterpart for a Win32
     * anonymous pipe, so dap_drain_output() would block on an empty pipe whose
     * write end is still open -- a hang, which is worse than the problem being
     * solved.
     *
     * Consequence: with out_pipe_r left at -1, drain is a no-op and the
     * debuggee's stdout goes to the real stdout, where it can interleave with
     * the JSON-RPC channel.  So the DAP debugger is effectively unsupported on
     * Windows until this is done properly with overlapped I/O (WIN3).
     */
#endif

    int rc = launch(program, args, n_args, s, ud);

    fflush(stdout);
    /* In replay mode the recorder already wrote the program's output through
     * to this pipe on its way into the recording. Draining it here would send
     * the whole transcript up front and then the replay would send it again as
     * the cursor advanced; the recording is the one that knows WHEN each byte
     * was printed, so it is the one that gets to say it. */
    if (!s->replay_mode) dap_drain_output(s);
    if (saved >= 0) { dup2(saved, STDOUT_FILENO); close(saved); }
    if (s->out_pipe_r >= 0) { close(s->out_pipe_r); s->out_pipe_r = -1; }

    if (s->replay_mode && s->trace) {
        size_t len = 0;
        const uint8_t *bytes = turi_trace_bytes(s->trace, &len);
        s->replay = bytes ? turi_trace_replay_open(bytes, len) : NULL;
        if (s->replay) {
            dap_replay_session(s);
            turi_trace_replay_free(s->replay);
            s->replay = NULL;
        }
    }
    if (s->trace) { turi_trace_end(s->trace); s->trace = NULL; }

    char body[64];
    snprintf(body, sizeof body, "{\"exitCode\":%d}", rc);
    dap_send_event(s, "exited", body);
    dap_send_event(s, "terminated", NULL);
}

int dap_server_run(int in_fd, int out_fd, DapLaunchFn launch, void *ud) {
    DapState st;
    memset(&st, 0, sizeof st);
    st.in_fd     = in_fd;
    st.rpc_fd    = dup(out_fd);  /* preserve real stdout before any redirect */
    st.seq       = 1;
    st.out_pipe_r = -1;

    bool   launched = false;
    char   program[4096] = {0};
    char **prog_args = NULL;
    int    n_prog_args = 0;

    for (;;) {
        char *msg = lsp_read_message(in_fd);
        if (!msg) break;  /* EOF -- client closed the connection */

        char command[64];
        if (lsp_json_str_copy(msg, "command", command, sizeof command) != 0) { free(msg); continue; }
        int64_t req_seq = lsp_json_int(msg, "seq");
        size_t alen;
        const char *araw = lsp_json_raw(msg, "arguments", &alen);
        char *args = NULL;
        if (araw) { args = (char *)malloc(alen + 1); memcpy(args, araw, alen); args[alen] = '\0'; }

        if (!strcmp(command, "initialize")) {
            dap_send_response(&st, req_seq, "initialize",
                "{\"supportsConfigurationDoneRequest\":true,"
                "\"supportsConditionalBreakpoints\":true,"
                "\"supportsEvaluateForHovers\":true,"
                /* Reverse execution. Advertised unconditionally, because the
                 * client asks for capabilities before it tells us whether this
                 * launch is a replay -- and a client that draws the buttons
                 * and gets an error on a live session is better than one that
                 * never offers them at all. A live session answers stepBack
                 * with "not supported while paused". */
                "\"supportsStepBack\":true,"
                "\"supportsReverseContinue\":true,"
                /* The timeline extension: `replayInfo` / `replaySeek` /
                 * `replayDepths`, and the `replayOutput` event. Not a DAP
                 * capability name, which is the point -- a client that does
                 * not recognise it will not ask, and everything it does not
                 * ask for degrades to the standard session it already
                 * understands. */
                "\"supportsTurmericReplayTimeline\":true,"
                "\"supportsTerminateRequest\":true}", true);
            dap_send_event(&st, "initialized", NULL);
        } else if (!strcmp(command, "launch")) {
            lsp_json_str_copy(args, "program", program, sizeof program);
            st.stop_on_entry = dap_json_bool(args, "stopOnEntry");
            /* T2: `"replay": true` records the run and then serves the session
             * from the recording, which is what makes stepBack and
             * reverseContinue answerable. Off by default -- a live session is
             * the one that can `evaluate`. */
            st.replay_mode   = dap_json_bool(args, "replay");
            n_prog_args = dap_parse_str_array(args, "args", &prog_args);
            launched = (program[0] != '\0');
            dap_send_response(&st, req_seq, "launch", NULL, launched);
            if (!launched)
                dap_send_error(&st, req_seq, "launch", "no program specified");
        } else if (!strcmp(command, "configurationDone")) {
            dap_send_response(&st, req_seq, "configurationDone", NULL, true);
            if (launched) {
                dap_run_program(&st, launch, ud, program, prog_args, n_prog_args);
                launched = false;  /* one program per session */
            }
        } else if (!strcmp(command, "disconnect") || !strcmp(command, "terminate")) {
            dap_send_response(&st, req_seq, command, NULL, true);
            free(args); free(msg);
            break;
        } else if (!dap_handle_common(&st, command, req_seq, args)) {
            dap_send_error(&st, req_seq, command, "unsupported request");
        }

        free(args);
        free(msg);
    }

    for (int i = 0; i < n_prog_args; i++) free(prog_args[i]);
    free(prog_args);
    if (st.rpc_fd >= 0) close(st.rpc_fd);
    return 0;
}
