#include "turi/trace.h"

#include "turi/eval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

/* --------------------------------------------------------------------------
 * Growable byte buffer
 *
 * Not Buf: this one holds arbitrary bytes with embedded NULs, and Buf's
 * string-shaped helpers (buf_puts, the NUL terminator) are the wrong
 * affordances for a format whose whole point is that a rendered value may
 * contain anything.
 * ----------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    bool     oom;
} Bytes;

static void bytes_reserve(Bytes *b, size_t extra) {
    if (b->oom) return;
    if (b->len + extra <= b->cap) return;
    size_t want = b->cap ? b->cap * 2 : 4096;
    while (want < b->len + extra) want *= 2;
    uint8_t *grown = realloc(b->data, want);
    if (!grown) { b->oom = true; return; }
    b->data = grown;
    b->cap  = want;
}

static void bytes_put(Bytes *b, const void *src, size_t n) {
    bytes_reserve(b, n);
    if (b->oom) return;
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void bytes_u8 (Bytes *b, uint8_t v)  { bytes_put(b, &v, 1); }
static void bytes_u16(Bytes *b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    bytes_put(b, t, 2);
}
static void bytes_u32(Bytes *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)(v >> 24) };
    bytes_put(b, t, 4);
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --------------------------------------------------------------------------
 * Interning
 * ----------------------------------------------------------------------- */

typedef struct {
    char    *text;
    uint16_t len;
} Name;

typedef struct {
    uint32_t file_name, fn_name, line, col, col_end;
} Site;

/* One frame's memo of what its locals last rendered as, so a STEP can carry
 * the difference rather than the state. */
typedef struct {
    char **names;   /* heap copies, owned */
    char **reprs;   /* heap copies, owned */
    int    n;
    int    cap;
} FrameMemo;

#define TRACE_MAX_DEPTH 512

struct TurTrace {
    TuriEnv  *env;
    Bytes     records;

    Name     *names;
    uint32_t  n_names, cap_names;
    Site     *sites;
    uint32_t  n_sites, cap_sites;

    FrameMemo memo[TRACE_MAX_DEPTH];

    uint32_t  max_steps;
    uint32_t  step_count;
    int       last_depth;
    bool      truncated;
    bool      stopped;
    TurTraceGrain grain;

    TurTraceStats stats;

    /* Serialized form, built on demand by turi_trace_bytes. */
    Bytes     serialized;
    bool      serialized_ok;

    /* Output capture. saved_stdout < 0 means capture is not active. */
    int       saved_stdout;
    int       pipe_read;

    /* Scratch used while enumerating one frame's locals. */
    struct {
        char **names;
        char **reprs;
        int    n, cap;
    } scan;
};

static uint32_t intern_name(TurTrace *t, const char *s) {
    if (!s) s = "";
    size_t len = strlen(s);
    if (len > 0xFFFF) len = 0xFFFF;
    for (uint32_t i = 0; i < t->n_names; i++) {
        if (t->names[i].len == (uint16_t)len &&
            memcmp(t->names[i].text, s, len) == 0)
            return i;
    }
    if (t->n_names == t->cap_names) {
        uint32_t want = t->cap_names ? t->cap_names * 2 : 64;
        Name *grown = realloc(t->names, want * sizeof *grown);
        if (!grown) return 0;
        t->names = grown;
        t->cap_names = want;
    }
    char *copy = malloc(len + 1);
    if (!copy) return 0;
    memcpy(copy, s, len);
    copy[len] = '\0';
    t->names[t->n_names].text = copy;
    t->names[t->n_names].len  = (uint16_t)len;
    return t->n_names++;
}

static uint32_t intern_site(TurTrace *t, const char *file, const char *fn,
                            uint32_t line, uint32_t col, uint32_t col_end) {
    uint32_t f = intern_name(t, file);
    uint32_t n = intern_name(t, fn);
    for (uint32_t i = 0; i < t->n_sites; i++) {
        Site *s = &t->sites[i];
        if (s->file_name == f && s->fn_name == n &&
            s->line == line && s->col == col && s->col_end == col_end)
            return i;
    }
    if (t->n_sites == t->cap_sites) {
        uint32_t want = t->cap_sites ? t->cap_sites * 2 : 256;
        Site *grown = realloc(t->sites, want * sizeof *grown);
        if (!grown) return 0;
        t->sites = grown;
        t->cap_sites = want;
    }
    t->sites[t->n_sites] = (Site){ f, n, line, col, col_end };
    return t->n_sites++;
}

/* --------------------------------------------------------------------------
 * Frame memo
 * ----------------------------------------------------------------------- */

static void memo_clear(FrameMemo *m) {
    for (int i = 0; i < m->n; i++) { free(m->names[i]); free(m->reprs[i]); }
    m->n = 0;
}

static void memo_free(FrameMemo *m) {
    memo_clear(m);
    free(m->names);
    free(m->reprs);
    m->names = m->reprs = NULL;
    m->cap = 0;
}

/* The previous rendering of `name` in this frame, or NULL. */
static const char *memo_get(const FrameMemo *m, const char *name) {
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->names[i], name) == 0) return m->reprs[i];
    return NULL;
}

static void memo_set(FrameMemo *m, const char *name, const char *repr) {
    for (int i = 0; i < m->n; i++) {
        if (strcmp(m->names[i], name) != 0) continue;
        char *copy = strdup(repr);
        if (!copy) return;
        free(m->reprs[i]);
        m->reprs[i] = copy;
        return;
    }
    if (m->n == m->cap) {
        int want = m->cap ? m->cap * 2 : 8;
        char **gn = realloc(m->names, (size_t)want * sizeof *gn);
        if (!gn) return;
        m->names = gn;
        char **gr = realloc(m->reprs, (size_t)want * sizeof *gr);
        if (!gr) return;
        m->reprs = gr;
        m->cap = want;
    }
    char *n = strdup(name), *r = strdup(repr);
    if (!n || !r) { free(n); free(r); return; }
    m->names[m->n] = n;
    m->reprs[m->n] = r;
    m->n++;
}

/* --------------------------------------------------------------------------
 * Output capture
 *
 * The interpreter's `println` writes to stdout with printf, so the only place
 * to intercept it is the descriptor. A pipe on fd 1, drained non-blockingly at
 * every node, keeps the transcript in step order without a thread: a node
 * cannot produce more than the pipe buffer holds before the next drain.
 * ----------------------------------------------------------------------- */

static void output_begin(TurTrace *t) {
    t->saved_stdout = -1;
    t->pipe_read    = -1;
#ifndef _WIN32
    int fds[2];
    if (pipe(fds) != 0) return;
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) { close(fds[0]); close(fds[1]); return; }
    if (dup2(fds[1], STDOUT_FILENO) < 0) {
        close(saved); close(fds[0]); close(fds[1]);
        return;
    }
    close(fds[1]);
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0) fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    t->saved_stdout = saved;
    t->pipe_read    = fds[0];
#endif
}

static void output_drain(TurTrace *t) {
#ifndef _WIN32
    if (t->pipe_read < 0) return;
    fflush(stdout);
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = read(t->pipe_read, buf, sizeof buf);
        if (n <= 0) break;
        bytes_u8(&t->records, TUR_TRACE_OUTPUT);
        bytes_u32(&t->records, (uint32_t)n);
        bytes_put(&t->records, buf, (size_t)n);
        t->stats.output_bytes += (uint32_t)n;
        /* Straight back out to the real stdout as well. The tracer is a
         * recorder, not a muzzle: a `tur trace` that swallowed what the
         * program printed would be reporting on a run the user cannot see. */
        if (t->saved_stdout >= 0) {
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(t->saved_stdout, buf + off, (size_t)(n - off));
                if (w <= 0) break;
                off += w;
            }
        }
        if ((size_t)n < sizeof buf) break;
    }
#else
    (void)t;
#endif
}

static void output_end(TurTrace *t) {
#ifndef _WIN32
    if (t->saved_stdout < 0) return;
    output_drain(t);
    fflush(stdout);
    dup2(t->saved_stdout, STDOUT_FILENO);
    close(t->saved_stdout);
    if (t->pipe_read >= 0) { close(t->pipe_read); t->pipe_read = -1; }
    t->saved_stdout = -1;
#endif
}

/* --------------------------------------------------------------------------
 * The pause handler
 * ----------------------------------------------------------------------- */

static void scan_local(const char *name, const char *repr, void *ud) {
    TurTrace *t = (TurTrace *)ud;
    if (!name) return;
    if (t->scan.n == t->scan.cap) {
        int want = t->scan.cap ? t->scan.cap * 2 : 16;
        char **gn = realloc(t->scan.names, (size_t)want * sizeof *gn);
        if (!gn) return;
        t->scan.names = gn;
        char **gr = realloc(t->scan.reprs, (size_t)want * sizeof *gr);
        if (!gr) return;
        t->scan.reprs = gr;
        t->scan.cap = want;
    }
    char *n = strdup(name);
    char *r = strdup(repr ? repr : "");
    if (!n || !r) { free(n); free(r); return; }
    t->scan.names[t->scan.n] = n;
    t->scan.reprs[t->scan.n] = r;
    t->scan.n++;
}

static void scan_reset(TurTrace *t) {
    for (int i = 0; i < t->scan.n; i++) {
        free(t->scan.names[i]);
        free(t->scan.reprs[i]);
    }
    t->scan.n = 0;
}

/* Ask for the next stop at whatever granularity this recording is being taken
 * at. Every early return in the handler goes through here too: a resume that
 * silently downgraded to step-in would leave the rest of the recording at a
 * different granularity than its header claims. */
static void trace_resume(TurTrace *t, TuriEnv *env) {
    if (t->grain == TUR_TRACE_GRAIN_LINE) turi_debug_resume_step_in(env);
    else                                  turi_debug_resume_step_node(env);
}

static void trace_pause(TuriEnv *env, TuriDbgStop reason, void *ud) {
    TurTrace *t = (TurTrace *)ud;
    (void)reason;

    if (t->stopped) { turi_debug_resume_continue(env); return; }

    if (t->step_count >= t->max_steps) {
        /* End the run the way a fuel exhaustion ends it, rather than letting
         * an untraced tail run on: the recording would then describe a prefix
         * of a program whose answer came from somewhere the trace cannot
         * show. Truncation is in the header, and every surface says so. */
        t->truncated = true;
        t->stopped   = true;
        turi_env_set_fuel(env, 1);
        turi_debug_resume_continue(env);
        return;
    }
    t->step_count++;

    int depth = turi_debug_frame_count(env);
    if (depth < 0) depth = 0;
    if (depth > (int)t->stats.peak_depth) t->stats.peak_depth = (uint32_t)depth;

    TuriDbgFrame f;
    memset(&f, 0, sizeof f);
    if (!turi_debug_frame_at(env, 0, &f)) {
        trace_resume(t, env);
        return;
    }
    uint32_t site = intern_site(t, f.file_path ? f.file_path : "",
                                f.fn_name ? f.fn_name : "", f.line,
                                f.col, f.end_col);

    /* Frame transitions first, so a decoder can maintain a stack without
     * inferring one from depth deltas of its own. */
    if (depth > t->last_depth) {
        for (int d = t->last_depth + 1; d <= depth; d++) {
            bytes_u8(&t->records, TUR_TRACE_ENTER);
            bytes_u32(&t->records, site);
            bytes_u16(&t->records, (uint16_t)(d > 0xFFFF ? 0xFFFF : d));
            t->stats.enters++;
        }
    } else if (depth < t->last_depth) {
        for (int d = t->last_depth; d > depth; d--) {
            bytes_u8(&t->records, TUR_TRACE_POP);
            bytes_u16(&t->records, (uint16_t)(d > 0xFFFF ? 0xFFFF : d));
            t->stats.pops++;
            if (d >= 0 && d < TRACE_MAX_DEPTH) memo_clear(&t->memo[d]);
        }
    }
    t->last_depth = depth;

    /* Only the innermost frame's locals: an outer frame cannot change while a
     * callee is executing, so recording it would repeat the whole stack on
     * every node of every call. */
    scan_reset(t);
    turi_debug_frame_locals(env, 0, scan_local, t);

    FrameMemo *m = (depth >= 0 && depth < TRACE_MAX_DEPTH)
                     ? &t->memo[depth] : NULL;

    /* Buffer the changes before writing, because the count is a prefix. */
    Bytes changes = {0};
    uint16_t n_changes = 0;
    for (int i = 0; i < t->scan.n; i++) {
        const char *prev = m ? memo_get(m, t->scan.names[i]) : NULL;
        if (prev && strcmp(prev, t->scan.reprs[i]) == 0) continue;
        size_t rlen = strlen(t->scan.reprs[i]);
        if (rlen > 0xFFFF) rlen = 0xFFFF;
        bytes_u32(&changes, intern_name(t, t->scan.names[i]));
        bytes_u16(&changes, (uint16_t)rlen);
        bytes_put(&changes, t->scan.reprs[i], rlen);
        if (m) memo_set(m, t->scan.names[i], t->scan.reprs[i]);
        n_changes++;
    }

    bytes_u8(&t->records, TUR_TRACE_STEP);
    bytes_u32(&t->records, site);
    bytes_u16(&t->records, (uint16_t)(depth > 0xFFFF ? 0xFFFF : depth));
    bytes_u16(&t->records, n_changes);
    if (changes.len) bytes_put(&t->records, changes.data, changes.len);
    free(changes.data);
    t->stats.steps++;
    t->stats.changes += n_changes;

    output_drain(t);
    trace_resume(t, env);
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

TurTrace *turi_trace_begin(TuriEnv *env, const TurTraceOpts *opts) {
    if (!env) return NULL;
    TurTrace *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->env          = env;
    t->max_steps    = (opts && opts->max_steps) ? opts->max_steps
                                                : TURI_TRACE_DEFAULT_MAX_STEPS;
    t->grain        = opts ? opts->grain : TUR_TRACE_GRAIN_NODE;
    t->last_depth   = 0;
    t->saved_stdout = -1;
    t->pipe_read    = -1;
    if (opts && opts->capture_output) output_begin(t);
    turi_debug_set_pause_handler(env, trace_pause, t);
    return t;
}

void turi_trace_stop(TurTrace *t) {
    if (!t) return;
    /* `stopped` may already be set -- the step cap sets it from inside the
     * handler -- but the handler itself is still installed until here. */
    t->stopped = true;
    if (t->env) {
        turi_debug_set_pause_handler(t->env, NULL, NULL);
        t->env = NULL;
    }
    output_end(t);
}

void turi_trace_end(TurTrace *t) {
    if (!t) return;
    turi_trace_stop(t);
    for (uint32_t i = 0; i < t->n_names; i++) free(t->names[i].text);
    free(t->names);
    free(t->sites);
    for (int i = 0; i < TRACE_MAX_DEPTH; i++) memo_free(&t->memo[i]);
    scan_reset(t);
    free(t->scan.names);
    free(t->scan.reprs);
    free(t->records.data);
    free(t->serialized.data);
    free(t);
}

void turi_trace_stats(TurTrace *t, TurTraceStats *out) {
    if (!t || !out) return;
    *out = t->stats;
    out->truncated = t->truncated;
}

const uint8_t *turi_trace_bytes(TurTrace *t, size_t *len_out) {
    if (!t) { if (len_out) *len_out = 0; return NULL; }
    if (!t->serialized_ok) {
        Bytes *b = &t->serialized;
        bytes_put(b, TURI_TRACE_MAGIC, TURI_TRACE_MAGIC_N);
        bytes_u16(b, TURI_TRACE_VERSION);
        uint8_t flags = 0;
        if (t->truncated)                        flags |= TUR_TRACE_FLAG_TRUNCATED;
        if (t->grain == TUR_TRACE_GRAIN_NODE)    flags |= TUR_TRACE_FLAG_NODE_GRAIN;
        bytes_u8 (b, flags);
        bytes_u32(b, t->n_names);
        for (uint32_t i = 0; i < t->n_names; i++) {
            bytes_u16(b, t->names[i].len);
            bytes_put(b, t->names[i].text, t->names[i].len);
        }
        bytes_u32(b, t->n_sites);
        for (uint32_t i = 0; i < t->n_sites; i++) {
            bytes_u32(b, t->sites[i].file_name);
            bytes_u32(b, t->sites[i].fn_name);
            bytes_u32(b, t->sites[i].line);
            bytes_u32(b, t->sites[i].col);
            bytes_u32(b, t->sites[i].col_end);
        }
        bytes_u32(b, (uint32_t)t->records.len);
        if (t->records.len) bytes_put(b, t->records.data, t->records.len);
        t->serialized_ok = !b->oom;
    }
    if (len_out) *len_out = t->serialized.len;
    return t->serialized.data;
}

/* --------------------------------------------------------------------------
 * Reading back
 * ----------------------------------------------------------------------- */

bool turi_trace_open(TurTraceReader *r, const uint8_t *bytes, size_t len) {
    if (!r || !bytes) return false;
    memset(r, 0, sizeof *r);
    size_t need = TURI_TRACE_MAGIC_N + 2 + 1 + 4;
    if (len < need) return false;
    if (memcmp(bytes, TURI_TRACE_MAGIC, TURI_TRACE_MAGIC_N) != 0) return false;
    size_t p = TURI_TRACE_MAGIC_N;
    r->bytes     = bytes;
    r->len       = len;
    r->version   = rd_u16(bytes + p); p += 2;
    /* v1 is still readable: it differs only in a narrower site record and the
     * absence of the granularity flag, and a recording downloaded from an
     * older tab should not stop being inspectable. */
    if (r->version == 1)                        r->site_bytes = TURI_TRACE_SITE_BYTES_V1;
    else if (r->version == TURI_TRACE_VERSION)  r->site_bytes = TURI_TRACE_SITE_BYTES_V2;
    else return false;
    uint8_t flags = bytes[p++];
    r->truncated  = (flags & TUR_TRACE_FLAG_TRUNCATED) != 0;
    r->node_grain = (flags & TUR_TRACE_FLAG_NODE_GRAIN) != 0;

    r->name_count = rd_u32(bytes + p); p += 4;
    r->names = bytes + p;
    for (uint32_t i = 0; i < r->name_count; i++) {
        if (p + 2 > len) return false;
        uint16_t n = rd_u16(bytes + p);
        p += 2u + n;
        if (p > len) return false;
    }

    if (p + 4 > len) return false;
    r->site_count = rd_u32(bytes + p); p += 4;
    r->sites = bytes + p;
    if (r->site_count > (len - p) / r->site_bytes) return false;
    p += (size_t)r->site_count * r->site_bytes;

    if (p + 4 > len) return false;
    r->record_bytes = rd_u32(bytes + p); p += 4;
    if (p + r->record_bytes > len) return false;
    r->records = bytes + p;
    r->cursor  = 0;
    return true;
}

const char *turi_trace_name(const TurTraceReader *r, uint32_t id,
                            uint16_t *len_out) {
    if (len_out) *len_out = 0;
    if (!r || id >= r->name_count) return "";
    const uint8_t *p = r->names;
    for (uint32_t i = 0; i < id; i++) p += 2u + rd_u16(p);
    uint16_t n = rd_u16(p);
    if (len_out) *len_out = n;
    return (const char *)(p + 2);
}

bool turi_trace_site(const TurTraceReader *r, uint32_t id, TurTraceSite *out) {
    if (!r || !out || id >= r->site_count) return false;
    const uint8_t *p = r->sites + (size_t)id * r->site_bytes;
    out->file_name = rd_u32(p);
    out->fn_name   = rd_u32(p + 4);
    out->line      = rd_u32(p + 8);
    out->col       = rd_u32(p + 12);
    /* A v1 site is a point, and 0 is how a client is told so. */
    out->col_end   = r->site_bytes >= TURI_TRACE_SITE_BYTES_V2 ? rd_u32(p + 16) : 0;
    return true;
}

bool turi_trace_next(TurTraceReader *r, TurTraceRecord *out) {
    if (!r || !out || r->cursor >= r->record_bytes) return false;
    const uint8_t *p = r->records + r->cursor;
    size_t left = r->record_bytes - r->cursor;
    memset(out, 0, sizeof *out);
    out->tag = *p;
    size_t used = 1;

    switch (out->tag) {
    case TUR_TRACE_ENTER:
        if (left < 1 + 4 + 2) return false;
        out->site  = rd_u32(p + 1);
        out->depth = rd_u16(p + 5);
        used = 1 + 4 + 2;
        break;
    case TUR_TRACE_STEP: {
        if (left < 1 + 4 + 2 + 2) return false;
        out->site      = rd_u32(p + 1);
        out->depth     = rd_u16(p + 5);
        out->n_changes = rd_u16(p + 7);
        used = 1 + 4 + 2 + 2;
        out->payload = p + used;
        size_t q = used;
        for (uint16_t i = 0; i < out->n_changes; i++) {
            if (q + 6 > left) return false;
            uint16_t rl = rd_u16(p + q + 4);
            q += 6u + rl;
            if (q > left) return false;
        }
        out->payload_len = (uint32_t)(q - used);
        used = q;
        break;
    }
    case TUR_TRACE_POP:
        if (left < 1 + 2) return false;
        out->depth = rd_u16(p + 1);
        used = 1 + 2;
        break;
    case TUR_TRACE_OUTPUT: {
        if (left < 1 + 4) return false;
        uint32_t n = rd_u32(p + 1);
        if (1u + 4u + n > left) return false;
        out->payload     = p + 5;
        out->payload_len = n;
        used = 5u + n;
        break;
    }
    default:
        return false;   /* unknown tag: the stream is not decodable past here */
    }
    r->cursor += used;
    return true;
}

bool turi_trace_change(const TurTraceRecord *rec, uint16_t i,
                       uint32_t *name_out, const char **repr_out,
                       uint16_t *repr_len_out) {
    if (!rec || rec->tag != TUR_TRACE_STEP || i >= rec->n_changes) return false;
    const uint8_t *p = rec->payload;
    size_t off = 0;
    for (uint16_t k = 0; k < i; k++) off += 6u + rd_u16(p + off + 4);
    if (name_out)     *name_out     = rd_u32(p + off);
    uint16_t rl = rd_u16(p + off + 4);
    if (repr_len_out) *repr_len_out = rl;
    if (repr_out)     *repr_out     = (const char *)(p + off + 6);
    return true;
}

/* --------------------------------------------------------------------------
 * Replay
 * ----------------------------------------------------------------------- */

typedef struct {
    uint32_t site;
    char   **names;
    char   **reprs;
    int      n, cap;
} RFrame;

struct TurTraceReplay {
    TurTraceReader rd;
    /* NUL-terminated copies of the interned names, so callers get C strings
     * rather than a pointer plus a length they would have to carry around. */
    char     **names;
    uint32_t   n_names;
    /* Record offset of each STEP, which is the axis a client scrubs. */
    size_t    *stops;
    uint32_t  *stop_sites;   /* site id of each stop, for breakpoint matching */
    uint32_t   n_stops;
    /* Depth at each stop, precomputed: step-over and step-out are "advance
     * until the depth comes back", and asking that question should not cost a
     * rebuild per candidate. */
    uint16_t  *depths;
    uint32_t   cursor;

    RFrame    *frames;
    int        n_frames, cap_frames;

    Bytes      output;
};

static void rframe_clear(RFrame *f) {
    for (int i = 0; i < f->n; i++) { free(f->names[i]); free(f->reprs[i]); }
    f->n = 0;
}

static void rframe_free(RFrame *f) {
    rframe_clear(f);
    free(f->names);
    free(f->reprs);
    f->names = f->reprs = NULL;
    f->cap = 0;
}

static void rframe_set(RFrame *f, const char *name, const char *repr,
                       uint16_t rlen) {
    for (int i = 0; i < f->n; i++) {
        if (strcmp(f->names[i], name) != 0) continue;
        char *copy = malloc((size_t)rlen + 1);
        if (!copy) return;
        memcpy(copy, repr, rlen);
        copy[rlen] = '\0';
        free(f->reprs[i]);
        f->reprs[i] = copy;
        return;
    }
    if (f->n == f->cap) {
        int want = f->cap ? f->cap * 2 : 8;
        char **gn = realloc(f->names, (size_t)want * sizeof *gn);
        if (!gn) return;
        f->names = gn;
        char **gr = realloc(f->reprs, (size_t)want * sizeof *gr);
        if (!gr) return;
        f->reprs = gr;
        f->cap = want;
    }
    char *n = strdup(name);
    char *r = malloc((size_t)rlen + 1);
    if (!n || !r) { free(n); free(r); return; }
    memcpy(r, repr, rlen);
    r[rlen] = '\0';
    f->names[f->n] = n;
    f->reprs[f->n] = r;
    f->n++;
}

static void replay_reset_frames(TurTraceReplay *rp) {
    for (int i = 0; i < rp->cap_frames; i++) rframe_clear(&rp->frames[i]);
    rp->n_frames = 0;
}

static bool replay_push_frame(TurTraceReplay *rp, uint32_t site) {
    if (rp->n_frames == rp->cap_frames) {
        int want = rp->cap_frames ? rp->cap_frames * 2 : 16;
        RFrame *grown = realloc(rp->frames, (size_t)want * sizeof *grown);
        if (!grown) return false;
        memset(grown + rp->cap_frames, 0,
               (size_t)(want - rp->cap_frames) * sizeof *grown);
        rp->frames = grown;
        rp->cap_frames = want;
    }
    rframe_clear(&rp->frames[rp->n_frames]);
    rp->frames[rp->n_frames].site = site;
    rp->n_frames++;
    return true;
}

/* Rebuild the state at stop `target` from the start of the stream. */
static void replay_rebuild(TurTraceReplay *rp, uint32_t target) {
    replay_reset_frames(rp);
    rp->output.len = 0;

    rp->rd.cursor = 0;
    uint32_t seen = 0;
    TurTraceRecord rec;
    while (turi_trace_next(&rp->rd, &rec)) {
        switch (rec.tag) {
        case TUR_TRACE_ENTER:
            replay_push_frame(rp, rec.site);
            break;
        case TUR_TRACE_POP:
            if (rp->n_frames > 0) {
                rframe_clear(&rp->frames[rp->n_frames - 1]);
                rp->n_frames--;
            }
            break;
        case TUR_TRACE_OUTPUT:
            bytes_put(&rp->output, rec.payload, rec.payload_len);
            break;
        case TUR_TRACE_STEP: {
            int depth = rec.depth;
            while (rp->n_frames < depth) replay_push_frame(rp, rec.site);
            if (rp->n_frames > depth) rp->n_frames = depth;
            if (rp->n_frames > 0) {
                RFrame *f = &rp->frames[rp->n_frames - 1];
                f->site = rec.site;
                for (uint16_t i = 0; i < rec.n_changes; i++) {
                    uint32_t nid = 0;
                    const char *repr = "";
                    uint16_t rl = 0;
                    if (!turi_trace_change(&rec, i, &nid, &repr, &rl)) break;
                    const char *bn = (nid < rp->n_names) ? rp->names[nid] : "";
                    rframe_set(f, bn, repr, rl);
                }
            }
            if (seen == target) return;   /* state AT this step, inclusive */
            seen++;
            break;
        }
        default:
            return;
        }
    }
}

TurTraceReplay *turi_trace_replay_open(const uint8_t *bytes, size_t len) {
    TurTraceReplay *rp = calloc(1, sizeof *rp);
    if (!rp) return NULL;
    if (!turi_trace_open(&rp->rd, bytes, len)) { free(rp); return NULL; }

    rp->names = calloc(rp->rd.name_count ? rp->rd.name_count : 1,
                       sizeof *rp->names);
    if (!rp->names) { free(rp); return NULL; }
    rp->n_names = rp->rd.name_count;
    for (uint32_t i = 0; i < rp->n_names; i++) {
        uint16_t nl = 0;
        const char *n = turi_trace_name(&rp->rd, i, &nl);
        char *copy = malloc((size_t)nl + 1);
        if (!copy) continue;
        memcpy(copy, n, nl);
        copy[nl] = '\0';
        rp->names[i] = copy;
    }

    /* Index the stop points and their depths in one pass. */
    uint32_t cap = 256;
    rp->stops      = malloc(cap * sizeof *rp->stops);
    rp->depths     = malloc(cap * sizeof *rp->depths);
    rp->stop_sites = malloc(cap * sizeof *rp->stop_sites);
    if (!rp->stops || !rp->depths || !rp->stop_sites) {
        turi_trace_replay_free(rp);
        return NULL;
    }
    rp->rd.cursor = 0;
    TurTraceRecord rec;
    for (;;) {
        size_t at = rp->rd.cursor;
        if (!turi_trace_next(&rp->rd, &rec)) break;
        if (rec.tag != TUR_TRACE_STEP) continue;
        if (rp->n_stops == cap) {
            cap *= 2;
            size_t *gs = realloc(rp->stops, cap * sizeof *gs);
            if (!gs) { turi_trace_replay_free(rp); return NULL; }
            rp->stops = gs;   /* adopt before the next realloc can fail */
            uint16_t *gd = realloc(rp->depths, cap * sizeof *gd);
            if (!gd) { turi_trace_replay_free(rp); return NULL; }
            rp->depths = gd;
            uint32_t *gt = realloc(rp->stop_sites, cap * sizeof *gt);
            if (!gt) { turi_trace_replay_free(rp); return NULL; }
            rp->stop_sites = gt;
        }
        rp->stops[rp->n_stops]      = at;
        rp->depths[rp->n_stops]     = rec.depth;
        rp->stop_sites[rp->n_stops] = rec.site;
        rp->n_stops++;
    }

    rp->cursor = 0;
    replay_rebuild(rp, 0);
    return rp;
}

void turi_trace_replay_free(TurTraceReplay *rp) {
    if (!rp) return;
    for (uint32_t i = 0; i < rp->n_names; i++) free(rp->names[i]);
    free(rp->names);
    free(rp->stops);
    free(rp->depths);
    free(rp->stop_sites);
    for (int i = 0; i < rp->cap_frames; i++) rframe_free(&rp->frames[i]);
    free(rp->frames);
    free(rp->output.data);
    free(rp);
}

uint32_t turi_trace_replay_steps(const TurTraceReplay *rp) {
    return rp ? rp->n_stops : 0;
}

uint32_t turi_trace_replay_index(const TurTraceReplay *rp) {
    return rp ? rp->cursor : 0;
}

uint32_t turi_trace_replay_seek(TurTraceReplay *rp, uint32_t index) {
    if (!rp || rp->n_stops == 0) return 0;
    if (index >= rp->n_stops) index = rp->n_stops - 1;
    rp->cursor = index;
    replay_rebuild(rp, index);
    return index;
}

int turi_trace_replay_frame_count(const TurTraceReplay *rp) {
    return rp ? rp->n_frames : 0;
}

bool turi_trace_replay_frame_at(const TurTraceReplay *rp, int idx,
                                TurTraceFrame *out) {
    if (!rp || !out || idx < 0 || idx >= rp->n_frames) return false;
    /* Index 0 is the innermost frame; the stack grows upwards internally. */
    const RFrame *f = &rp->frames[rp->n_frames - 1 - idx];
    TurTraceSite site;
    memset(out, 0, sizeof *out);
    out->fn_name   = "";
    out->file_path = "";
    if (turi_trace_site(&rp->rd, f->site, &site)) {
        if (site.file_name < rp->n_names && rp->names[site.file_name])
            out->file_path = rp->names[site.file_name];
        if (site.fn_name < rp->n_names && rp->names[site.fn_name])
            out->fn_name = rp->names[site.fn_name];
        out->line    = site.line;
        out->col     = site.col;
        out->col_end = site.col_end;
    }
    return true;
}

int turi_trace_replay_local_count(const TurTraceReplay *rp, int idx) {
    if (!rp || idx < 0 || idx >= rp->n_frames) return 0;
    return rp->frames[rp->n_frames - 1 - idx].n;
}

bool turi_trace_replay_local_at(const TurTraceReplay *rp, int idx, int i,
                                const char **name_out, const char **repr_out) {
    if (!rp || idx < 0 || idx >= rp->n_frames) return false;
    const RFrame *f = &rp->frames[rp->n_frames - 1 - idx];
    if (i < 0 || i >= f->n) return false;
    if (name_out) *name_out = f->names[i];
    if (repr_out) *repr_out = f->reprs[i];
    return true;
}

const char *turi_trace_replay_output(const TurTraceReplay *rp, size_t *len_out) {
    if (!rp) { if (len_out) *len_out = 0; return ""; }
    if (len_out) *len_out = rp->output.len;
    return rp->output.data ? (const char *)rp->output.data : "";
}

int turi_trace_replay_depth_at(const TurTraceReplay *rp, uint32_t index) {
    if (!rp || index >= rp->n_stops) return 0;
    return rp->depths[index];
}

bool turi_trace_replay_site_at(const TurTraceReplay *rp, uint32_t index,
                               const char **file_out, uint32_t *line_out) {
    if (file_out) *file_out = "";
    if (line_out) *line_out = 0;
    if (!rp || index >= rp->n_stops) return false;
    TurTraceSite site;
    if (!turi_trace_site(&rp->rd, rp->stop_sites[index], &site)) return false;
    if (file_out && site.file_name < rp->n_names && rp->names[site.file_name])
        *file_out = rp->names[site.file_name];
    if (line_out) *line_out = site.line;
    return true;
}

/* The trailing path component, so a breakpoint set on "input.tur" matches a
 * site recorded with an absolute path -- the same basename rule the live
 * debugger's breakpoint table uses. */
static const char *replay_basename(const char *p) {
    const char *slash = strrchr(p, '/');
#ifdef _WIN32
    const char *back = strrchr(p, '\\');
    if (back && (!slash || back > slash)) slash = back;
#endif
    return slash ? slash + 1 : p;
}

static bool replay_stop_matches(const TurTraceReplay *rp, uint32_t index,
                                const char *file, uint32_t line) {
    const char *fp = "";
    uint32_t at = 0;
    if (!turi_trace_replay_site_at(rp, index, &fp, &at)) return false;
    if (at != line) return false;
    if (file && *file && strcmp(replay_basename(fp), file) != 0) return false;
    return true;
}

uint32_t turi_trace_replay_find_line(const TurTraceReplay *rp, int dir,
                                     const char *file, uint32_t line,
                                     bool *hit_out) {
    if (hit_out) *hit_out = false;
    if (!rp || rp->n_stops == 0) return 0;
    uint32_t last = rp->n_stops - 1;
    if (dir >= 0) {
        for (uint32_t i = rp->cursor + 1; i <= last; i++) {
            if (!replay_stop_matches(rp, i, file, line)) continue;
            if (hit_out) *hit_out = true;
            return i;
        }
        return last;
    }
    for (uint32_t i = rp->cursor; i > 0; i--) {
        if (!replay_stop_matches(rp, i - 1, file, line)) continue;
        if (hit_out) *hit_out = true;
        return i - 1;
    }
    return 0;
}
