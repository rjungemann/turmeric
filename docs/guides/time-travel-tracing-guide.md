---
title: Time-Travel Tracing Guide (`tur trace`)
category: Editor and IDE
description: Recording an interpreted run and scrubbing it -- what the .turtrace format holds, what the step cap protects, and how `tur dap` replays a recording so a debugger can step backwards
---

# Time-Travel Tracing Guide (`tur trace`)

`tur trace <file.tur>` runs a program under the interpreter with a recorder
attached, writing every node it evaluates into a byte buffer. The recording can
then be read back, summarised, or replayed as a debugging session that steps
**backwards**.

```sh
tur trace hello.tur                        # record; print a summary
tur trace hello.tur -o run.turtrace        # record; write the bytes
tur trace hello.tur --max-steps=50000      # cap the recording
tur trace --dump run.turtrace              # read one back, record by record
```

## Why record, when the debugger already pauses

`tur debug` and `tur dap` both pause a live program today, with frames, locals
and in-frame `evaluate`. So the case for a recording is not that a pause is
impossible. It is three other things:

1. **Backwards.** The question a debugger cannot answer is *how did this value
   come to be 7*, and stepping back is the answer. A pause cannot go back.
2. **It is nearly free to build.** The pause handler is already called at every
   node and already has an API for frames and locals, so
   `turi_debug_set_pause_handler` plus resume-step-in in a loop *is* a tracer.
   There is no source instrumentation anywhere in this: the interpreter is
   ours, and `turi_debug_frame_at` gives real frames rather than sentinel
   addresses that have to be ordered by guesswork.
3. **The web has no debugger.** `tur dap` is stdio, and a browser tab has no
   way to host a blocking pause loop. A trace is a byte buffer, and a byte
   buffer crosses the wasm boundary and is scrubbed client-side with no
   protocol at all.

The recorder is an **interpreter** feature. Compiled programs are not traced; a
native tracer is a different job and would start from the `#line` emission
behind `--debug`.

## What a recording holds

```
header   "TURTRACE\0"        9 bytes
         u16 version         (1)
         u8  flags           bit 0: truncated
         u32 name_count      name[name_count]
         u32 site_count      site[site_count]
         u32 record_bytes    record[] filling exactly that many bytes
name     u16 len, u8 bytes[len]
site     u32 file_name, u32 fn_name, u32 line, u32 col
record   u8 tag
  1 ENTER   u32 site, u16 depth
  2 STEP    u32 site, u16 depth, u16 n, change[n]
  3 POP     u16 depth
  4 OUTPUT  u32 len, u8 text[len]
change   u32 name, u16 len, u8 repr[len]
```

All integers are little-endian. Everything that would otherwise repeat -- file
paths, function names, binding names -- is interned in the name table, so
nothing crosses as a string at run time. It crosses as *bytes* rather than as a
C string because a rendered value may contain a NUL, and this is the one thing
here that reaches megabytes.

Three properties are worth stating outright:

- **Deltas, not states.** A `STEP` carries only the bindings whose rendered
  value changed since that frame's last step. A step carrying every live
  variable repeats the whole frame on every pass of a loop; in practice one or
  two values move per node. Measured on a 20,000-iteration loop: 80,006 steps
  in 1.2 MB, about 15 bytes a step.
- **No keyframes.** A decoder builds its own snapshots if it wants them, which
  is the same work in a language that can afford it, and keeps the recorder to
  one rule: write what changed.
- **Output is interleaved.** The program's stdout is captured through a pipe
  and drained at every node, so `OUTPUT` records sit in step order and a scrub
  backwards rewinds the transcript with the cursor. It is forwarded to the real
  stdout on the way past -- the recorder is a recorder, not a muzzle.
  (Unavailable on Windows, where the non-blocking pipe read this relies on has
  no counterpart; the recording simply carries no `OUTPUT` records there.)

Only the *innermost* frame's locals are recorded per step. An outer frame
cannot change while a callee is executing, so recording it would repeat the
whole stack on every node of every call.

## The step cap, which is not optional

A recording of a runaway loop is a tab that dies. `--max-steps=N` (default
200,000) ends the run through the same unwind a fuel exhaustion takes, rather
than letting an untraced tail run on -- a recording that describes a prefix of
a program whose answer came from somewhere it cannot show is worse than a short
one.

**Truncation is reported, never silent.** The header carries a `truncated`
flag, the summary line says `truncated yes`, and `--dump` prints it.

One trap worth knowing: the tree-walking interpreter retains roughly 4 KiB per
step of a trampolined loop, so a 1e6-step program already peaks at ~3.5 GiB RSS
*without* a tracer. Trace small programs; the cap is what keeps them small.

## Cost

Measured on a Debug build with ASan, on a 20,000-iteration `while` loop:
untraced 0.11s, traced 0.57s -- roughly **5x**. The constant is dominated by
the locals enumeration, which happens per *node*. If that matters for a
particular program, the shape of the cheaper version is to capture only on
frame entry/exit and on `let` / `set!` nodes, which is a smaller trace and a
coarser scrub.

Values that do not render get their type tag and `?`, which is the same honest
answer `turi_try_show_by_tag` already gives. Prelude and stdlib loading are not
recorded: recording starts where the debugger arms.

## Reverse execution over DAP

`tur dap` launched with `"replay": true` records the whole run and then serves
the debug session **from the recording**:

```json
{ "command": "launch",
  "arguments": { "program": "hello.tur", "stopOnEntry": true, "replay": true } }
```

`stackTrace`, `scopes` and `variables` answer from a trace cursor rather than
from a live frame, which is what makes `stepBack`, `reverseContinue` and
`reverseNext` answerable at all. The server advertises `supportsStepBack` and
`supportsReverseContinue`, and VS Code and nvim-dap draw the entire
reverse-execution UI off those -- the scrubber, the backwards breakpoints, the
rewinding variables pane -- so nothing here is a widget.

Two deliberate differences from a live session:

- **`evaluate` refuses.** There is no live frame to evaluate in, and saying so
  beats returning a stale value that looks like an answer. Relaunch without
  `"replay"` for a session that can.
- **Conditional breakpoints become unconditional.** A condition is an
  expression in a frame, for the same reason. That stops more often than asked,
  never less.

Stepping backwards does not un-print: a terminal has no undo, so the DAP
transcript only ever grows. The transcript that rewinds with the cursor belongs
to a client that owns its own console.

Replay is opt-in. A plain `launch` is still a live session, still the one that
can `evaluate`, and unchanged.

## Reading a recording in C

`src/turi/trace.h` carries a reader (`turi_trace_open` / `turi_trace_next` /
`turi_trace_name` / `turi_trace_site` / `turi_trace_change`) and a replay
(`turi_trace_replay_*`) that reconstructs the frame stack and each frame's
locals at any step.

The replay rebuilds from the start of the stream on every seek rather than
undoing deltas backwards. That is O(records) per seek -- a few milliseconds at
the default cap -- and it is the difference between a decoder that is obviously
correct and one that has to get an undo log right in both directions.

**Per seek** is the operative phrase. Asking *where was step N* is
`turi_trace_replay_site_at`, which reads an index and does not seek; asking it
by seeking, once per candidate, is how a scan to the end of an 80k-step
recording goes from 0.02 seconds to not finishing. Anything that walks the step
axis looking for something -- breakpoint matching, a search -- reads the index.

## See also

- [lsp-guide.md](lsp-guide.md) -- the language server, including scope-aware
  highlight, rename and references.
- `docs/artifacts/debugger-dap-phase3.md` -- the live DAP session this replays
  alongside. Referenced as a path rather than linked: the offline docs pack
  carries guides and API modules only, and `genpack.py --strict-links` rejects
  a link that would leave the pack.
