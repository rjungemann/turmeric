# Turmeric Debugger -- Incremental Plan

Status: sketch / proposed
Owner: unassigned
Track: post-v1 polish; phases 1-2 may land opportunistically before v1.

## Goal

Give turmeric programmers a real debugger experience -- breakpoints, stepping,
locals inspection, stack traces -- without blocking on a full native DWARF
story. Stage the work so each phase ships value on its own and the next phase
extends rather than replaces it.

## Why incremental

Turmeric has two execution paths: the tree-walking interpreter (`turi`/eval,
used by `tur run <file>` in interp mode, the REPL, and many tests) and
`emit-C` -> native compilation. Each can host a debugger at very different
cost. The interpreter path can carry a working debugger in days; native
debugging requires DWARF type emission for opaques/ADTs/options/results and
is multi-week. Splitting the work means programmers get step/break/inspect
early (against the interpreter) while the native side ramps.

## Non-goals

- Time-travel / reverse debugging.
- Profiler / sampler (separate effort).
- Remote debugging over the network beyond what DAP gives for free.
- Replacing `tur repl` -- the debugger augments it, not the other way around.

## Phases

### Phase 1 -- Source spans audit

**Status: landed.** Audit write-up + the span-coverage gate are in
[debugger-spans-audit.md](./debugger-spans-audit.md). `tur audit-spans <file>`
walks the post-elaboration tree and reports breakpoint-eligible nodes (defn,
let form, call site, top-level form) lacking a usable span; the
`tur_span_coverage` ctest target (`tests/check-span-coverage.sh`) gates the
whole fixture suite (1434 clean / 0 holes). The interpreter `Value`/closure
span field and emitted-C `#line` directives are deferred to Phases 2 and 4
respectively (their consumers).

**Outcome:** every AST node that can be a breakpoint target or stack frame
carries `{file, line, col, end_line, end_col}` through elaboration and into
both the interpreter's eval nodes and the C emitter.

- Inventory current span coverage: parser, macro expansion, elaboration
  passes, monomorphization, lowering. Most diagnostics already need this --
  list the holes.
- Preserve spans across macro expansion (record original site + expansion
  site; the debugger shows the original, stack traces can show both).
- Add a span field to interpreter `Value` closures and to emitted C function
  metadata so a runtime frame can resolve back to a source location.

**Exit criteria:** for every fixture under `tests/fixtures/`, every defn,
let-binding, call site, and top-level form in the AST has a non-zero span
that round-trips through elaboration.

**Risk:** low. Mostly plumbing; a few macro/quasiquote cases will need care.

### Phase 2 -- Interpreter debugger (CLI)

**Status: landed.** Write-up in
[debugger-interpreter-phase2.md](./debugger-interpreter-phase2.md). `tur debug
<file>` drops into a `(tur-dbg)` REPL with break / step / next / finish /
continue / backtrace / locals / print / list / quit and the `(break)` builtin.
The eval-loop hooks live in `src/turi/eval.c` behind a single `env->debugger`
pointer (a NULL check on the hot path, so non-debugger interp runs are
unaffected); the `tur_debugger` ctest target (`tests/run-debugger.sh`) drives a
scripted session over `tests/fixtures/debugger/` and asserts the output.

**Outcome:** `tur debug <file>` drops into a debugger REPL with break,
step (in/over/out), continue, backtrace, locals/args inspection, and a
`(break)` builtin that triggers a breakpoint from source.

- Add a `Debugger` struct owned by the eval loop with: pending step mode,
  breakpoint table keyed by `(file, line)`, current frame pointer.
- Hook the eval dispatch: before evaluating a node, if its span hits a
  breakpoint or the step predicate fires, yield to the debugger REPL.
- Inspect commands read directly from the active `Env` -- locals are already
  named there. Print using existing value printer + type info.
- Source listing: read source by file from spans, print +/- N lines around
  current location.
- Conditional breakpoints: parse the condition as a turmeric expr, eval in
  the paused frame's env.
- `(break)` builtin: compiles to a no-op in native; in interp it forces a
  pause at the call site.

**Exit criteria:** scripted CLI session against a fixture program demonstrates
break/step/inspect/continue; a regression fixture under
`tests/fixtures/debugger/` drives the debugger from a script and asserts
output.

**Risk:** medium. The eval loop hot path takes a per-node check (cheap if the
breakpoint table is empty); benchmark to confirm no regression in
non-debugger interp runs.

### Phase 3 -- DAP server over the interpreter

**Status: landed.** Write-up in
[debugger-dap-phase3.md](./debugger-dap-phase3.md). `tur dap` speaks the Debug
Adapter Protocol (JSON-RPC 2.0 / stdio, the same transport as `tur lsp`) as a
thin shell over Phase 2's debugger. Phase 2's `TuriDebugger` gained a typed
control API (`turi_debug_*` in `src/turi/eval.h`): a pause handler that replaces
the text REPL on a stop, resume controls, breakpoint-table management, and
frame / locals / evaluate introspection. The server (`src/turi/dap.c`) maps DAP
requests onto that API; the `tur_dap` ctest target
(`tests/run-dap.sh` + `tests/dap-driver.py`) drives a scripted JSON-RPC session
over `tests/fixtures/dap/`. A VS Code extension stub ships in
[editors/vscode-turmeric/](../../editors/vscode-turmeric/).

**Outcome:** VS Code, Neovim DAP, and any other DAP client can attach to a
turmeric program running under the interpreter and get full breakpoint /
step / locals UI.

- Implement the Debug Adapter Protocol (JSON-RPC over stdio) as a thin shell
  around Phase 2's debugger API.
- Map DAP requests (`setBreakpoints`, `stackTrace`, `scopes`, `variables`,
  `stepIn`, etc.) to debugger calls.
- Ship a VS Code extension stub (`launch.json` config + the DAP transport)
  so the out-of-box experience is one command + one config block.

**Exit criteria:** screenshot/demo of VS Code stepping through a turmeric
program, inspecting locals (including structs, options, results), and
hitting a conditional breakpoint. *(Demonstrated by the `tur_dap` regression's
transcript: a scripted client steps through the fixture, inspects per-frame
locals + `evaluate`, and hits a conditional breakpoint that fires only when
`i == 3`. Struct / option / result locals render inline via the value printer;
expandable trees are deferred -- see the Phase 3 write-up.)*

**Risk:** low-medium. DAP is well-documented but verbose; the work is
mechanical once Phase 2 exposes the right primitives.

### Phase 4 -- Source maps for emit-C (native gdb/lldb minimal)

**Status: landed.** Write-up in
[debugger-native-sourcemaps-phase4.md](./debugger-native-sourcemaps-phase4.md).
`tur build --debug` threads `#line N "file.tur"` directives into the generated C
(via `emit_line_directive` at the function-entry and per-statement chokepoints,
gated on `g_emit_debug_lines` so default codegen / `emit-c` snapshots are
unchanged) and compiles single-file targets with `-g -Og`, so gdb/lldb step
through `.tur` source and a crash backtrace names turmeric symbols at turmeric
locations. The `tur_phase4_gdb` ctest target (`tests/run-phase4-gdb.sh`) asserts
both the gated `#line` emission and a turmeric-source gdb backtrace of a
deliberate crash over `tests/fixtures/debugger-phase4/`; it skips the gdb half
cleanly when gdb is absent. (`-Og`, not `-O0`: a self-contained single-file
build relies on optimizer DCE to drop preloaded stdlib defns that reference
libturi-only symbols it does not link -- see the write-up.)

**Outcome:** binaries built via `tur build` carry `#line` directives in
their generated C, so gdb/lldb step through `.tur` source files (showing
turmeric lines, not the C carrier) and stack traces print turmeric symbols
+ turmeric source locations.

- Emit `#line N "path/to/file.tur"` at every span boundary in the C
  emitter. Keep the emitted C still human-readable by also leaving comments
  for the transitions during debug builds.
- Use turmeric-friendly C symbol names (already partly true) so backtraces
  read as turmeric.
- Document the workflow: `tur build --debug foo.tur && gdb ./build/bin/foo`.
- Acceptance: a fixture program with a deliberate crash shows a
  turmeric-source backtrace under gdb.

**Caveat:** locals will still render as raw C carriers (`int64_t`, tagged
union internals). Phase 5 fixes that. Phase 4 alone is already a large win
for native crash triage.

**Risk:** low. `#line` is the oldest debugger trick in C; the only subtle
part is making sure macro-expanded code points back to the user's source,
not the macro definition.

### Phase 5 -- Rich type display in native debuggers

Split into its own plan:
[debugger-native-types-plan.md](./debugger-native-types-plan.md).

Short version: ship Python pretty-printers for gdb and lldb on top of the
C-DWARF the C compiler already emits, plus a small VS Code integration via
CodeLLDB / lldb-dap. Avoid emitting DWARF directly from the Turmeric
compiler -- Rust's experience is that even with "correct" variant-part
DWARF, you still need the printers, so skip the multi-month detour.

### In-frame expression evaluator (cross-cutting; split-out plan)

Split into its own plan:
[debugger-inframe-eval-plan.md](./debugger-inframe-eval-plan.md).

Not a sequential phase -- a cross-cutting enhancement to the Phase 2 / 3
interpreter track. Both phases ship a *narrow shim* for evaluating in a paused
frame: Phase 2's `print <name>` and Phase 3's `evaluate`/hover resolve a single
binding, and DAP conditional breakpoints accept only `<name> <op> <literal>`.
The split-out plan generalizes that to evaluating an **arbitrary Turmeric
expression** in a paused frame's lexical scope, which unlocks full-expression
conditional breakpoints (`break <line> if <expr>`) and arbitrary `evaluate`.
The hard part is recovering enough type information from the paused frame's
runtime values to re-elaborate a fresh expression against them.

## Dependencies between phases

- Phase 2 depends on Phase 1.
- Phase 3 depends on Phase 2.
- Phase 4 depends on Phase 1 only (parallel with 2/3).
- Phase 5 depends on Phase 4.
- The in-frame expression evaluator (split-out plan) depends on Phase 2 and
  Phase 3; it supersedes the single-name / simple-comparison shims those phases
  shipped.

So Phase 1 + Phase 4 can land in parallel with the 2 -> 3 interpreter track.

## Open questions

- Does the eval loop's per-node debugger check measurably regress
  non-debugger interp runs? (Benchmark before/after Phase 2.)
- Macro spans: when a breakpoint sits inside macro-expanded code, do we
  pause at the call site, the expansion site, or both? (Probably: pause at
  call site by default; `step in` descends into expansion.)
- Do we want a `tur debug --attach <pid>` mode for the native debugger, or
  is launch-only acceptable for v1 of Phase 5?
- Source-map format for the web/WASM build (`web/turmeric.js`) -- emit JS
  source maps from emit-C? Deferred until someone asks.

## Suggested first PR

Phase 1 audit + a span-coverage test that walks every fixture's AST and
asserts no zero spans on breakpoint-eligible nodes. Cheap, unblocks the
rest, and surfaces holes the diagnostics already work around silently.
