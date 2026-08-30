#ifndef TURI_TRACE_H
#define TURI_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "turi/env.h"

/* ---------------------------------------------------------------------------
 * Time-travel tracer for the interpreter
 * (editor-intelligence-follow-through-plan, track T)
 *
 * Records every node the interpreter evaluates into a byte buffer that can be
 * scrubbed forwards and backwards after the fact.
 *
 * Why a recording, when `tur debug` and `tur dap` already pause a live
 * program:
 *
 *   1. Backwards. The question a debugger cannot answer is "how did this value
 *      come to be 7", and stepping back is the answer. A pause cannot go back.
 *   2. It is nearly free to build. The pause handler is already called at
 *      every node and already has an API for frames and locals, so
 *      turi_debug_set_pause_handler plus resume-step-in in a loop IS a tracer.
 *      There is no source instrumentation here at all -- the interpreter is
 *      ours, and turi_debug_frame_at gives real frames rather than sentinel
 *      addresses that have to be ordered by guesswork.
 *   3. The web has no debugger. `tur dap` is stdio, and a browser tab has no
 *      way to host a blocking pause loop. A trace is a byte buffer, and a byte
 *      buffer crosses the wasm boundary and is scrubbed client-side with no
 *      protocol at all.
 *
 * Deltas, not states: a step carries only the bindings whose rendered value
 * changed since that frame's last step. A step carrying every live variable
 * repeats the whole frame on every pass of a loop, and in practice one or two
 * values move per node.
 *
 * There are no keyframes in the format. A decoder builds its own snapshots
 * every N steps, which is the same work in a language that can afford it and
 * keeps this side to one rule: write what changed.
 * --------------------------------------------------------------------------- */

/* A recording of a runaway loop is a tab that dies. */
#ifndef TURI_TRACE_DEFAULT_MAX_STEPS
#  define TURI_TRACE_DEFAULT_MAX_STEPS 200000u
#endif

typedef struct TurTrace TurTrace;

typedef struct {
    /* 0 selects TURI_TRACE_DEFAULT_MAX_STEPS. Reaching it ends the run through
     * the same unwind a fuel exhaustion takes, and sets the header's
     * `truncated` flag -- truncation is reported, never silent. */
    uint32_t max_steps;
    /* Interleave the program's stdout into the record stream, so a scrub
     * backwards rewinds the transcript with the cursor. Costs a pipe on fd 1
     * for the duration of the run; unavailable on Windows, where it is
     * silently off. */
    bool     capture_output;
} TurTraceOpts;

/* Install the recorder on `env`.
 *
 * Takes over the debugger's pause handler, so it is mutually exclusive with a
 * DAP or REPL session on the same env. The caller arms the debugger as usual
 * afterwards; every stop from then on is captured and immediately resumed
 * step-in, so the program runs to completion without ever stopping for a
 * user. */
TurTrace *turi_trace_begin(TuriEnv *env, const TurTraceOpts *opts);

/* Stop recording (restores stdout if it was captured) and free the trace.
 * Any pointer from turi_trace_bytes is invalidated. */
void turi_trace_end(TurTrace *t);

/* Stop recording without freeing, so the bytes and stats can still be read.
 * Called automatically by turi_trace_bytes and turi_trace_stats. */
void turi_trace_stop(TurTrace *t);

typedef struct {
    uint32_t steps;         /* STEP records */
    uint32_t enters;        /* ENTER records */
    uint32_t pops;          /* POP records */
    uint32_t changes;       /* individual binding changes recorded */
    uint32_t peak_depth;    /* deepest frame count seen */
    uint32_t output_bytes;  /* program stdout captured */
    bool     truncated;     /* the step cap ended the run */
} TurTraceStats;

void turi_trace_stats(TurTrace *t, TurTraceStats *out);

/* The serialized recording. Valid until turi_trace_end.
 *
 * Bytes rather than a C string because this is the one thing here that
 * reaches megabytes, and a NUL inside a rendered value would truncate it. */
const uint8_t *turi_trace_bytes(TurTrace *t, size_t *len_out);

/* ---------------------------------------------------------------------------
 * The format
 *
 *   header   "TURTRACE\0"        9 bytes
 *            u16 version         TURI_TRACE_VERSION
 *            u8  flags           bit 0: truncated
 *            u32 name_count      name[name_count]
 *            u32 site_count      site[site_count]
 *            u32 record_bytes    record[] filling exactly that many bytes
 *   name     u16 len, u8 bytes[len]
 *   site     u32 file_name, u32 fn_name, u32 line, u32 col
 *   record   u8 tag
 *     1 ENTER   u32 site, u16 depth
 *     2 STEP    u32 site, u16 depth, u16 n, change[n]
 *     3 POP     u16 depth
 *     4 OUTPUT  u32 len, u8 text[len]
 *   change   u32 name, u16 len, u8 repr[len]
 *
 * Everything that would otherwise repeat -- file paths, function names,
 * binding names -- is interned in the name table, so nothing crosses as a
 * string at run time.
 *
 * Two deliberate departures from the plan's sketch: ids are u32 rather than
 * u16 (a 65k ceiling on distinct names is the kind of limit that is fine until
 * a generated program walks into it), and ENTER/POP carry no separate frame
 * index -- `depth` already identifies the frame, because frames are a stack.
 * All integers are little-endian.
 * --------------------------------------------------------------------------- */

#define TURI_TRACE_MAGIC   "TURTRACE\0"
#define TURI_TRACE_MAGIC_N 9
#define TURI_TRACE_VERSION 1

enum {
    TUR_TRACE_ENTER  = 1,
    TUR_TRACE_STEP   = 2,
    TUR_TRACE_POP    = 3,
    TUR_TRACE_OUTPUT = 4,
};

/* --- Reading back ------------------------------------------------------- */

typedef struct {
    const uint8_t *bytes;
    size_t         len;
    uint16_t       version;
    bool           truncated;
    /* Name table: offsets into `bytes`, resolved by turi_trace_name. */
    const uint8_t *names;
    uint32_t       name_count;
    const uint8_t *sites;
    uint32_t       site_count;
    const uint8_t *records;
    size_t         record_bytes;
    /* Cursor into `records`, advanced by turi_trace_next. */
    size_t         cursor;
} TurTraceReader;

typedef struct {
    uint8_t     tag;
    uint32_t    site;      /* ENTER, STEP */
    uint16_t    depth;     /* ENTER, STEP, POP */
    uint16_t    n_changes; /* STEP */
    /* STEP: the change list, decoded lazily by turi_trace_change.
     * OUTPUT: the captured text. */
    const uint8_t *payload;
    uint32_t       payload_len;
} TurTraceRecord;

/* Parse the header. Returns false on a bad magic, an unknown version, or a
 * buffer that is shorter than its own tables claim. */
bool turi_trace_open(TurTraceReader *r, const uint8_t *bytes, size_t len);

/* Read the next record at the cursor. Returns false at the end. */
bool turi_trace_next(TurTraceReader *r, TurTraceRecord *out);

/* Resolve an interned name. Returns "" for an out-of-range id, never NULL.
 * The result points into the caller's buffer and is NOT NUL-terminated:
 * *len_out is the length. */
const char *turi_trace_name(const TurTraceReader *r, uint32_t id,
                            uint16_t *len_out);

typedef struct {
    uint32_t file_name;
    uint32_t fn_name;
    uint32_t line;
    uint32_t col;
} TurTraceSite;

bool turi_trace_site(const TurTraceReader *r, uint32_t id, TurTraceSite *out);

/* Decode change `i` of a STEP record. Returns false when i is out of range. */
bool turi_trace_change(const TurTraceRecord *rec, uint16_t i,
                       uint32_t *name_out, const char **repr_out,
                       uint16_t *repr_len_out);

/* ---------------------------------------------------------------------------
 * Replay -- the state a recording describes at any one of its steps
 *
 * The recording is deltas, so "what did the stack look like at step 4218" is a
 * question only a replay can answer. This is that replay, in C rather than in
 * the DAP server, because two consumers want it: `tur dap`'s reverse execution
 * (T2) and, eventually, the browser timeline (T3).
 *
 * Seeking rebuilds from the start of the stream rather than undoing deltas
 * backwards. That is O(records) per seek, which at the default 200k-step cap
 * is a few milliseconds, and it is the difference between a decoder that is
 * obviously correct and one that has to get an undo log right in both
 * directions. The plan's "the decoder builds its own snapshots every N steps"
 * is the optimization to reach for if that stops being true.
 * --------------------------------------------------------------------------- */

typedef struct TurTraceReplay TurTraceReplay;

/* Open a replay over `bytes`, which the caller must keep alive for the
 * replay's lifetime. NULL if the buffer is not a readable recording. */
TurTraceReplay *turi_trace_replay_open(const uint8_t *bytes, size_t len);
void            turi_trace_replay_free(TurTraceReplay *rp);

/* Stop points -- one per STEP record. This is the axis a client scrubs. */
uint32_t turi_trace_replay_steps(const TurTraceReplay *rp);
uint32_t turi_trace_replay_index(const TurTraceReplay *rp);

/* Seek to `index`, clamped into range. Returns the index actually reached. */
uint32_t turi_trace_replay_seek(TurTraceReplay *rp, uint32_t index);

/* The frame stack at the cursor. Index 0 is the INNERMOST frame, matching
 * turi_debug_frame_at, so a DAP stackTrace reads the same either way. */
typedef struct {
    const char *fn_name;    /* NUL-terminated; owned by the replay */
    const char *file_path;
    uint32_t    line;
    uint32_t    col;
} TurTraceFrame;

int  turi_trace_replay_frame_count(const TurTraceReplay *rp);
bool turi_trace_replay_frame_at(const TurTraceReplay *rp, int idx,
                                TurTraceFrame *out);

/* The locals of frame `idx` at the cursor, in the order they were first seen.
 * A binding appears once, carrying its most recent rendering. */
int  turi_trace_replay_local_count(const TurTraceReplay *rp, int idx);
bool turi_trace_replay_local_at(const TurTraceReplay *rp, int idx, int i,
                                const char **name_out, const char **repr_out);

/* Program output produced strictly before the cursor's step, as one buffer.
 * *len_out is its length; the bytes are owned by the replay and are rebuilt on
 * every seek. */
const char *turi_trace_replay_output(const TurTraceReplay *rp, size_t *len_out);

/* Search for a step whose site is on `file` (basename match; "" matches any)
 * at `line`, scanning forward (`dir` > 0) or backward from the cursor. Returns
 * the index found, or the boundary index when there is no hit -- which is what
 * a `continue` with no breakpoint ahead of it should do. `*hit_out` says which
 * of the two happened. */
uint32_t turi_trace_replay_find_line(const TurTraceReplay *rp, int dir,
                                     const char *file, uint32_t line,
                                     bool *hit_out);

/* The depth (frame count) at an arbitrary step index, without seeking.
 * Returns 0 for an out-of-range index. */
int turi_trace_replay_depth_at(const TurTraceReplay *rp, uint32_t index);

/* The source position of an arbitrary step, without seeking.
 *
 * Without-seeking is the whole point: matching a breakpoint means asking this
 * of every step between here and the next hit, and a seek rebuilds the state
 * from the start of the stream. Asking it that way turns a `continue` over a
 * 200k-step recording into 200k rebuilds -- which is not slow, it is a hang.
 * `*file_out` points into the replay and is NUL-terminated. */
bool turi_trace_replay_site_at(const TurTraceReplay *rp, uint32_t index,
                               const char **file_out, uint32_t *line_out);

#endif
