# Try Turmeric: the time-travel timeline (track T3)

> **Status: Executed (2026-08-30).** T3.0 through T3.5 all landed; see
> [§10 Execution record](#10-execution-record). Written 2026-08-30 against
> v0.41.0.
> **Type:** WASM glue / web client.
> **Executes:** T3 of
> [`editor-intelligence-follow-through-plan.md`](editor-intelligence-follow-through-plan.md),
> which landed T1 (`tur trace`) and T2 (DAP reverse execution) on 2026-08-30
> and deferred T3 behind them on purpose: "if the format is wrong, T3 is the
> expensive place to find that out" (§4.5). The format now has two consumers
> and a version byte, so that gate is met.
> **Prior art, built and shipped:** c2mp's
> `~/Projects/c2mir-playground/c2mp/docs/debugger-plan.md` -- a recording
> debugger with a scrubber, a gutter and a variables panel, in a browser, over
> a wasm interpreter. Status line there reads "Built." This plan is the same
> product with the two hardest sections deleted, because Turmeric does not
> need them.

## 0. Summary

Try Turmeric gets a second run mode. **Trace** records the program under the
interpreter, hands the recording to the page as bytes, and turns the console
area into a scrubber: step forward and backward over the run, watch the editor
gutter follow the cursor, and read each live frame's bindings at that point.

Every byte of this already exists in C and is exercised by two shipping
consumers. What is missing is a wasm export, a worker message, and a panel.

---

## 1. Where this actually stands today

An audit, because the tracing guide reads as though the browser half exists
and it does not.

**The C side is complete.** `src/turi/trace.h` carries the recorder
(`turi_trace_begin` / `_stats` / `_bytes`), the reader (`turi_trace_open` /
`_next` / `_name` / `_site` / `_change`) and a **full replay** --
`turi_trace_replay_open`, `_seek`, `_frame_at`, `_local_at`, `_output`,
`_find_line`, `_depth_at`, `_site_at`. The replay's own header comment names
this plan's consumer in advance: "two consumers want it: `tur dap`'s reverse
execution (T2) and, eventually, the browser timeline (T3)."

**The recorder is already in the wasm source set.** `turi/trace.c` sits in
`TURI_EVAL_SOURCES` (`src/CMakeLists.txt:117`), which is spliced into
`TUR_CORE_SOURCES` (:289), which is `WASM_SOURCES` (:1525). It compiles for
wasm32 today. Nothing in `-sEXPORTED_FUNCTIONS` (:1573) reaches it, though --
that list ends `_turi_explain,_malloc,_free` and mentions no trace symbol, so
the module carries the recorder and offers no way to call it.

**Nothing above that line exists.** Verified rather than assumed:

| Layer | File | State |
|---|---|---|
| Export list | `src/CMakeLists.txt:1573` | no trace symbol |
| WASM glue | `src/web/wasm_glue.c` | the string "trace" does not appear in the file |
| Worker protocol | `web/public/eval-worker.js:39` | `eval`, `format`, `doc`, `type-of`, `explain`, `lang-registry`, `reset` -- no trace |
| Page | `web/main.js` (5332 lines) | no trace UI, no `.turtrace` handling |
| Deployed artifact | `web/public/turmeric.wasm` | last built 2026-07-29, which **predates the tracer entirely** |

So the honest answer to "how do I run the tracer in Try Turmeric" is: you
cannot, and no partial version of it is in the tab either.

**One doc consequence.** The tracing guide's §3, "The web has no debugger ...
a byte buffer crosses the wasm boundary and is scrubbed client-side with no
protocol at all", is the *rationale for choosing a flat byte format*. It is
easy to read as a description of shipped behavior. When this plan lands the
guide gets a paragraph that says how to actually do it; until then the
sentence stays as written, because it is an argument, not a claim.

---

## 2. What c2mp proves, and what Turmeric does not have to build

c2mp built this against a C interpreter with no debug hook of any kind. Its
plan is 290 lines and **more than half of them are two problems Turmeric does
not have**:

| c2mp section | Cost there | Cost here |
|---|---|---|
| §3 The instrumenter -- a text pass over C inserting hook calls, three brace/semicolon rules, two safety valves | The largest section; "a text pass over C will meet constructs nobody anticipated" | **Zero.** The interpreter is ours. `turi_debug_set_pause_handler` fires at every node already. No source is rewritten. |
| §4 Which call is this? -- sentinel addresses, serial stamping, sweeping from the bottom | "the source of every bug worth recording"; one bug cost a 93 MB trace of an 18-frame program | **Zero.** `turi_debug_frame_at` returns real frames. `trace.h` notes ENTER/POP need no frame index at all, because depth identifies the frame. |
| §5 The recording format | Built | **Built here too**, and already versioned |
| §5 decoder + cursor + snapshots (`vite-wasm/src/debug.js`, 358 lines) | Built in JS | **Replaced by the C replay** -- see §3 |
| The panel, the gutter, the prompt commands | Built | **This plan.** The only part left. |

That is why T3 is a small plan and T1 was not. The load-bearing claim from the
archived plan holds: "Where c2mp had to build a language server, a scope
analysis and every widget by hand, Turmeric already owns the elaborated tree,
a full LSP, a DAP server and Monaco."

---

## 3. The one architectural decision: replay in C, not a decoder in JS

c2mp decodes the trace in JavaScript (`debug.js`) and keeps its own snapshot
every 500 steps. Copying that here would mean a **second decoder for a format
that already has a tested one**, and the format is exactly the kind of thing
two decoders drift on -- c2mp needed `test-debug.js` (392 lines) to hold its
single decoder to the encoder.

**Decision: the browser calls `turi_trace_replay_*` through wasm.** The page
never parses a `.turtrace` byte. The trace buffer crosses once, stays in wasm
memory, and the UI asks the same replay that answers `tur dap`'s `stepBack`.
One decoder, already covered by T1/T2's tests.

The cost is a worker round-trip per seek. Two facts make that fine:

- A seek is O(records) by design -- "at the default 200k-step cap is a few
  milliseconds" (`trace.h`) -- and `_site_at` / `_depth_at` exist precisely so
  breakpoint search and the timeline's depth ribbon do **not** seek.
- Slider drags coalesce to one seek per animation frame. The measured trace is
  1.2 MB for 80k steps, so this is arithmetic on a buffer already in memory,
  not I/O.

If a seek ever measures slow enough to feel, the fallback is the optimization
`trace.h` already names (snapshots every N steps) -- added in C, where it
benefits DAP too, rather than in a JS decoder that only benefits the tab.

---

## 4. Surfaces

### 4.1 WASM glue (`src/web/wasm_glue.c`)

Follow the file's existing shape: `EMSCRIPTEN_KEEPALIVE` on the export
(:569, :659, :732 are the pattern), strings out through
`turi_wasm_strdup` / `turi_wasm_free_string`.

```c
/* Record. Returns the recording's length; the bytes are fetched separately
 * because they are bytes -- a NUL in a rendered value truncates a string. */
int         turi_wasm_trace_run(const char *input, uint32_t max_steps);
const uint8_t *turi_wasm_trace_bytes(size_t *len_out);

/* Replay over the recording just made. */
uint32_t turi_wasm_trace_steps(void);
uint32_t turi_wasm_trace_seek(uint32_t index);
/* Frames, locals, output and the site of an arbitrary index, as JSON --
 * wasm_glue.c already builds JSON by hand for turi_wasm_lang_registry
 * (wasm_json_escape, :470), so this needs no new machinery. */
const char *turi_wasm_trace_state(void);
const char *turi_wasm_trace_site_at(uint32_t index);
uint32_t    turi_wasm_trace_find_line(int dir, const char *file, uint32_t line);
```

`turi_wasm_trace_run` replicates what `cmd_trace` does at `src/main.c:7502`:
begin the recorder, run with the debugger armed, and -- this is the part that
must not be skipped -- **`turi_trace_stop` before returning**. The recorder
takes over the env's pause handler (`trace.h`), and the web REPL reuses one
env across every subsequent eval. A handler left installed turns the whole
session into a slow one.

Add the new symbols to `-sEXPORTED_FUNCTIONS` (`src/CMakeLists.txt:1573`).

**Open item, resolve first:** `turi_debug_enable(env, FILE *in, FILE *out)`
(`src/turi/eval.h:94`) is the CLI's arm point and takes two `FILE *`. What it
wants under emscripten with no console to prompt at -- NULL, or the real
stdio -- decides whether this is a three-line function or needs a
handler-only arm path next to it.

### 4.2 Worker (`web/public/eval-worker.js`)

One more branch in `handleMessage` (:39), matching the `eval` branch above it.
The difference is the return: the bytes come out of `HEAPU8` and are
**copied and transferred**, not stringified. c2mp learned this the hard way
and wrote it down: "`ccall(..., 'string')` would stop at the first NUL", and
the buffer must be copied out immediately "because the buffer behind it is a
`std::string` the next call may move."

Here the buffer belongs to a `TurTrace` that lives until the next
`turi_wasm_trace_run`, which is a longer life than c2mp's -- but copy anyway,
for the same reason.

Scrub messages (`trace-seek`, `trace-find-line`) are ordinary request/response
and resolve through the existing `pendingCalls` map (`web/main.js:1090-1140`).

### 4.3 The page (`web/main.js`, `web/try/index.html`)

- **A Trace button** beside Run (`try/index.html:45`). A second button, not a
  mode Run always takes, for c2mp's reason: the recording costs roughly 5x
  (measured, §6) and produces megabytes.
- **A timeline strip** in the console pane (`try/index.html:293`), above the
  transcript: a range input over `[0, steps)`, step-back / step-forward
  buttons, and `file:line` for the cursor. The split pane and its handle
  (:271) already exist.
- **Frames and locals** in the doc-panel slot (`close-doc-btn`, :327) -- a
  slide-in side panel with a close button is a pattern the page already
  ships, so this is a second instance rather than new layout.
- **Gutter follow.** A Monaco decoration on the cursor's line, and a
  click-a-line-to-jump binding driven by `turi_trace_replay_find_line`. This
  is the "backwards breakpoints" affordance without a breakpoint model: the
  recording is finite, so "next hit on line 12" is a scan.
- **Console replay.** The transcript renders `turi_trace_replay_output`, so
  scrubbing backwards rewinds the output with the cursor. This is what the
  OUTPUT record exists for.
- **A `:trace` meta-command** in `REPL_META_COMMANDS` (`web/main.js:4518`) for
  prompt parity, the way c2mp put its commands in `meta.js`. The `:help` text
  generates from that table, so it costs one row.

---

## 5. Phases

| Phase | Contents | Gate to the next |
|---|---|---|
| **T3.0** | `turi_wasm_trace_run` + `_bytes`, export list, worker branch. No UI: print the stats line to the console. | A 10-line program traced in the tab reports a nonzero step count and a byte length, and the **next plain eval is not slowed** (the pause handler came off) |
| **T3.1** | Replay exports + `trace-seek`. Still no UI: a `:trace` command that seeks and prints the frame stack. | For one program, the frames and locals at step N in the browser are identical to `tur trace` + a `--dump` read of the same source natively |
| **T3.2** | The timeline strip: slider, step buttons, `file:line`, gutter decoration. | Scrubbing a `fib` recording end to end moves the gutter monotonically and never exceeds `peak_depth` |
| **T3.3** | Frames + locals panel; console output replay. | Stepping backward into a returned frame shows that frame's values (T2's gate, re-asserted through the page) |
| **T3.4** | Click-a-line jump via `_find_line`; truncation banner. | A line with no hit ahead lands on the boundary and says so, rather than appearing to hang |
| **T3.5** | Playwright spec + the parity test | §8 |

T3.0 -> T3.1 is a hard chain; the rest are ordered by usefulness, not by
dependency. The only file shared with any other track is `web/main.js`, which
the archived plan already flagged as the one collision surface (§5).

---

## 6. Cost, and the three risks worth naming

**Measured, from the T1/T2 execution record:** a 20,000-iteration `while` loop
ran 0.11s untraced and 0.57s traced (**~5x**), producing 80,006 steps in
**1.2 MB** -- about 15 bytes a step. That was a Debug + ASan build; the wasm
build is `-O2` and no Release number has been taken. It sits inside c2mp's
measured 1.6x-13x band.

**Risk 1: output capture may not survive emscripten.** `capture_output` works
by `pipe()` + `dup2()` onto `STDOUT_FILENO` (`src/turi/trace.c:234-240`),
guarded only against `_WIN32`. In the tab, program output reaches the user
through Emscripten's `print` callback, which the worker forwards as
`{type:'print'}` messages. If `dup2` redirects fd 1 into the pipe, live
console output **goes silent for the duration of a traced run** -- which is
arguably correct (the transcript is replayed from the trace instead), but it
is a behavior change the UI has to expect rather than discover. Verify in
T3.0. Fallback: run with `capture_output = false` in the browser and let the
console stream live, losing only the rewinding transcript.

**Risk 2: memory, not speed, is the browser's limit.** CLAUDE.md's standing
warning applies with full force here: the tree-walking interpreter retains
roughly 4 KiB per step of a trampolined loop, so a 1e6-step program peaks
around 3.5 GiB *without* a tracer. The 200,000-step default cap
(`TURI_TRACE_DEFAULT_MAX_STEPS`) was chosen for a native process. **Default
the browser to something smaller -- 50,000 is a 750 KB trace and a defensible
tab** -- and expose the cap in the UI rather than hard-coding one number.
Truncation is already reported in the header flag; surface it as a banner, never
silently.

**Risk 3: wasm size.** The trace and replay code is dead-stripped today.
Exporting it keeps it. Measure the delta on the first build; if it is large
enough to hurt first paint, the answer is a second module, not a smaller
tracer -- but measure before assuming there is a problem.

---

## 7. Not in scope

- **Tracing compiled programs.** The recorder is an interpreter feature; the
  tab runs the interpreter. A native tracer is a different plan that starts
  from `#line` emission behind `--debug`.
- **Evaluating an expression at the cursor.** There is no live frame in a
  recording. T2 already decided this for DAP -- `evaluate` returns an error
  naming the reason rather than a stale value -- and the page says the same.
- **Loading a `.turtrace` file from disk.** The format is stable and the
  reader is exported, so a drop-target is cheap later; it is not what "run the
  tracer in Try Turmeric" means.
- **Tracing prompt-defined functions**, matching c2mp §7's exclusion for the
  same REPL-module reason.

---

## 8. Testing

- **Parity, the load-bearing test.** The same source, traced natively via
  `tur trace -o` and traced in the browser, must produce the **same step count
  and the same frame stack at the same index**. c2mp shipped exactly this
  (`test-debug-parity.js`, and it runs every editor example twice); it is the
  test that catches a wasm32 divergence nobody would otherwise look for.
- **A fixture** under `tests/fixtures/`, carrying `requires.interp-only` --
  the trace is an interpreter behavior and `run.sh`'s compiled path does not
  share it, which is the rule T1's fixture already follows.
- **Playwright**, in `web/tests/`. Note the standing environment problem from
  the T1/T2 execution record: those specs need `web/turmeric.js`, building it
  needs emscripten, and the execution environment did not have it -- the
  existing web specs were written but never run. Building the wasm is a
  prerequisite for this phase, not an afterthought.

## 9. Left

- Struct/collection bindings rendered as more than a summary line.
- A depth ribbon under the slider (c2mp did not build one either; it is the
  cheapest way to see recursion shape, and `_depth_at` exists to feed it
  without seeking).
- Sharing a recording through the existing `?code=` URL state. The trace is
  megabytes, so this needs a store, not an encoding.

---

## 10. Execution record

Executed 2026-08-30 on branch `worktree-try-turmeric-tracer-t3`. All six phases
landed. `emcc` was present in this environment, so unlike the T1/T2 execution
the Playwright half actually ran.

### 10.1 What shipped

| Phase | Landed as |
|---|---|
| T3.0 | `turi_wasm_trace_run` / `_stats` + the export list + the worker's `trace-run` branch |
| T3.1 | `_seek` / `_state` / `_site_at` / `_find_line` / `_buffer` / `_release`, and `trace-seek` |
| T3.2 | The timeline strip: slider, first/back/forward/last, `file:line`, gutter decoration |
| T3.3 | Frames + locals panes; console output replayed from the recording |
| T3.4 | Line-number click jumps to that line's next execution (Alt+click for previous); truncation banner |
| T3.5 | `web/tests/trace.spec.js` -- 8 specs, all green, including native-vs-browser parity |

### 10.2 The three risks, resolved

**Risk 1 (output capture under emscripten) did not materialize.** `pipe`,
`dup`, `dup2` and `fcntl(O_NONBLOCK)` all work in the Emscripten POSIX layer,
so `capture_output` is on in the browser and the OUTPUT records are populated.
`trace.c`'s decision to write drained bytes back out to the saved descriptor
("the tracer is a recorder, not a muzzle") pays off here in a way it was not
designed for: the console still streams live *during* a traced run, so the
transcript restored when the timeline closes is the real one.

**Risk 3 (wasm size) is zero.** Measured by building the module twice, with a
reconfigure between so the export list actually changed: **3,475,137 bytes both
ways.** The recorder and the replay were being linked in already; the export
list only decided whether anything could call them. §1's claim that they were
"dead-stripped" was wrong and has been corrected -- the deployed artifact was
simply built on 2026-07-29, before the tracer existed.

**Risk 2 (memory) stands as designed.** `TRACE_MAX_STEPS` is 50,000 in
`web/main.js`, a quarter of the recorder's native default, and truncation
raises a banner.

### 10.3 One thing the plan did not anticipate

**Interpreter line numbers are not editor line numbers.** The browser env
accumulates every eval into `env->src_acc` and hands the whole blob to the
reader, so a site's line is absolute in that blob -- a five-line tab reports
its errors at `<eval>:75`, and a timeline lighting up line 75 of a five-line
file would be worse than no timeline at all. The bridge captures
`env->acc_next_line` before the run and reports it as `baseLine`; the page
subtracts. Measured on the `fib` program: `baseLine` 62, sites at 63/65/68,
editor lines 2/4/7 -- which is what the gutter highlights.

**A recording is a run of a whole program, so it needs a whole environment.**
Reported from a local server the day this landed: pressing Trace a second time
died with `defeffect: 'Ask' is already defined`. The browser env is a REPL
session that accumulates every eval, so the second Trace was re-evaluating the
program on top of the first one's definitions. `tur trace <file>` is a new
process with an empty env and the browser now matches it -- Trace resets the
session first, which also makes two recordings of one program comparable
(identical step counts and an identical `baseLine`, which the regression spec
asserts). Run deliberately does not reset, because Run is how a tab's
definitions become callable at the prompt; the two buttons want opposite
things from the env and now each gets it.

The same fix carried the `#lang` directive tail into `trace-run`, which had
been missed: `turi_eval_typed` strips an inline `#lang` itself, but `set_lang`
*assigns* the layer set, so a program opening `#lang turmeric stringed` would
have recorded with its layers off.

**And one behavior worth stating.** `turi_trace_replay_output` answers "what
had been printed *before* the cursor's step", which is right everywhere except
the end: a program whose last act is a `println` drains it after the final
STEP record, so the transcript at the last step was empty for a program that
had visibly printed. Rather than change the replay -- its semantics are correct
for its own question and `tur dap` depends on them -- the glue got
`turi_wasm_trace_output_full`, built on the public reader, which the page asks
for only when the cursor is on the last step.

### 10.4 Verification

- `web/tests/trace.spec.js`: **10 passed.** The parity spec records
  `tests/fixtures/trace/input.tur` with `./build/tur trace` and the same
  program in the tab: **65 steps both ways**, peak depth 7.
- `bash tests/run-trace.sh`: **19 passed, 0 failed** -- the native T1 harness is
  unaffected.
- Full desktop Playwright suite: 127 passed. The failures that remain
  (minimap x2, repl-intelligence x3, smoke "Force update") reproduce on an
  untouched checkout and are pre-existing. A further batch (guide-toggle x7,
  smoke's console-error assertion) is a fresh worktree missing generated web
  assets -- `web/public/doc-names.json` and the docs HTML -- not a regression;
  copying `doc-names.json` in turns the `:doc`-completion spec green.
- `:help` is generated from `REPL_META_COMMANDS`, so adding `:trace` needed no
  test update and the alignment spec still passes.

### 10.5 Left, unchanged from §9

The depth ribbon, struct/collection bindings rendered as more than a summary
line, and sharing a recording through the URL. `turi_wasm_trace_site_at`
already returns `depth` without seeking, so the ribbon is the cheapest of the
three.

One doc follow-up landed with this: the tracing guide gained a "Recording in
the browser" section, since §1's note about its §3 reading as shipped behavior
stopped being hypothetical the moment this did ship.
