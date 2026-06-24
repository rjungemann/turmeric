# Debugger Phase 2 -- Interpreter Debugger (CLI)

Status: landed
Track: [debugger-plan.md](./debugger-plan.md), Phase 2

This is the Phase 2 deliverable from [debugger-plan.md](./debugger-plan.md): an
interactive debugger over the tree-walking interpreter. `tur debug <file.tur>`
runs a program under the interpreter and drops into a command REPL with
breakpoints, stepping, a call stack, and locals inspection -- all keyed on the
`{file, line}` spans Phase 1 verified are present on every breakpoint-eligible
node.

## Using it

```sh
tur debug path/to/program.tur [args...]
```

The program stops at its first executable node (the entry stop) and prints a
`(tur-dbg)` prompt. Commands are read from stdin, so a session can be scripted
by piping a command stream (this is exactly how the regression test drives it).

| Command | Aliases | Effect |
| --- | --- | --- |
| `break <line>` | `b` | breakpoint on a line of the current file |
| `break <file>:<line>` | `b` | breakpoint on a line of a named file (basename match) |
| `delete [n]` | `d` | remove breakpoint `n` (all if omitted) |
| `continue` | `c` | run to the next breakpoint / `(break)` |
| `step` | `s` | step to the next source line (descends into calls) |
| `next` | `n` | step over calls (stay at this depth or shallower) |
| `finish` | `fin`, `out` | run until the current function returns |
| `backtrace` | `bt`, `where` | print the call stack |
| `locals` | `l` | print every binding in lexical scope |
| `print <name>` | `p` | print one binding's value |
| `list` | `ls` | show a source window around the current line |
| `quit` | `q` | abort the program |
| `help` | `h`, `?` | command summary |

The `(break)` builtin triggers a breakpoint from source: under `tur debug` it
forces a pause at the call site; under plain `tur --interpret` (no debugger
attached) it is a no-op, so a program peppered with `(break)` still runs
normally.

## How it works

A `TuriDebugger` is attached to the `TuriEnv` by `turi_debug_enable()` and lives
behind a single `env->debugger` pointer (`src/turi/env.h`). All the logic is in
`src/turi/eval.c` alongside the evaluator internals (`EvalFrame`, the value
printer, `eval_lookup`) it depends on.

### Where the hooks sit

The interpreter has two evaluation paths -- the recursive `eval_expr` and the
explicit-stack driver `eval_drive_ex` (which folds nested control flow and
tail-call chains onto a heap work-stack). A node can be evaluated by either, so
the per-node check is mirrored in both:

- `eval_expr` calls `turi_dbg_before_node(..., from_driver=false)` before it
  dispatches a node.
- `eval_drive_ex` calls it (`from_driver=true`) at the top of its
  control-node descent, for the nested `if` / `do` / `let` / `call` / `match`
  bodies that never pass through `eval_expr`.

`eval_expr` delegates the foldable kinds (`let`/`if`/`do`/`program`/`call`/
`match`) straight to the driver, so the same node would be hooked twice in a
row; the debugger records that node in `skip_node` on the `eval_expr` pass and
swallows the driver's immediate duplicate.

When no debugger is attached the hook is a single `if (env->debugger)`
NULL-pointer load on the eval hot path -- no measurable cost on plain runs
(answering the benchmark concern in the plan's open questions).

### The call stack

`turi_dbg_push` / `turi_dbg_pop` / `turi_dbg_set_top` bracket each turi-body
activation. They are wired into the driver's `DK_CALL_RET` lifecycle: a push at
each activation creation (the `eval_apply_driven` seed, the non-tail fold, and
the native-resume fold), a `set_top` on a tail call (which reuses the
activation in place, so backtrace depth stays O(1) like the runtime), and a pop
at the `DK_CALL_RET` epilogue. Each frame records its function name, its
lexical frame (for `locals`), and the span of the node currently executing in
it (for the backtrace location). The depth this maintains also backs the
`next` / `finish` predicates.

### Stepping

Stepping is line-granular. The debugger remembers the line it last stopped on
and the depth it stepped from:

- `step` stops at the next node on a different line, any depth.
- `next` adds the constraint *depth <= step depth*, so a call on the current
  line runs to completion before we stop again.
- `finish` stops once *depth < step depth*, i.e. after the current frame
  returns.

A line breakpoint fires on *entry* to the line (a transition from a different
source line), not once per node that happens to share the line -- the debugger
tracks the previously executed node's line to make that distinction.

### Source fidelity

Under the debugger the user file is brought in via `(load "path")` instead of
the default concatenate-into-`<eval>` path, so it keeps its own `file_id` and
real 1-based line numbers. Without this every line would be offset by the
preloaded prelude and source listings would point at the synthetic `<eval>`
blob.

## Tests

- `tests/run-debugger.sh` (ctest target `tur_debugger`) drives a scripted
  session over `tests/fixtures/debugger/input.tur` and asserts the output for
  break / step / next / finish / backtrace / locals / print / `(break)` /
  continue / quit.
- The fixture carries `requires.dedicated-runner`, so `tests/run.sh`
  PASS-skips it (the debug REPL and the `(break)` builtin only exist on the
  interpreter path; the compiled path has no `break`).

## Not in this phase

- DAP server / editor integration -- Phase 3.
- Native (`emit-C`) debugging -- Phases 4-5.
- Conditional breakpoints (`break <line> if <expr>`): evaluating an arbitrary
  expression in the paused frame's lexical scope is deferred to the in-frame
  expression evaluator ([debugger-inframe-eval-plan.md](./debugger-inframe-eval-plan.md)).
  `print <name>` covers single-binding inspection in the meantime. (Phase 3's
  DAP server adds a `<name> <op> <literal>` conditional shim on top of this,
  which the split-out plan generalizes.)
