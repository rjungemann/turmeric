# Debugger Phase 4 -- Native Source Maps for emit-C

Status: landed
Track: [debugger-plan.md](./debugger-plan.md), Phase 4

This is the Phase 4 deliverable from [debugger-plan.md](./debugger-plan.md):
native source maps for the `emit-C` -> compiled path. `tur build --debug`
threads `#line N "path/to/file.tur"` directives into the generated C, so
gdb/lldb step through the **Turmeric** source -- not the C carrier -- and a
crash backtrace prints Turmeric symbols at Turmeric source locations.

Unlike Phases 2 and 3 (which sit over the tree-walking interpreter), Phase 4 is
entirely on the AOT/native side. It deliberately stops short of rich type
display: locals still render as their raw C carriers (`int64_t`, tagged-union
internals). That is Phase 5's job
([debugger-native-types-plan.md](./debugger-native-types-plan.md)). Phase 4
alone is already a large win for native crash triage.

## Using it

```sh
tur build --debug foo.tur -o foo   # #line directives + -g -Og
gdb ./foo
(gdb) run
(gdb) bt
```

A backtrace of a crash reads as Turmeric:

```
#5  boom   (n=42) at foo.tur:2
#6  middle (x=41) at foo.tur:6
#7  main   (...)  at foo.tur:10
```

`--debug` is a global flag, so it works on any subcommand. On `emit-c` it lets
you inspect the directives directly:

```sh
tur --debug emit-c foo.tur | grep '#line'
```

Without `--debug`, codegen is byte-for-byte unchanged -- the 80-odd
`tests/fixtures/*/expected.c` snapshots do not move, and ordinary release
builds are unaffected.

## What `--debug` does

| Aspect | Default | `--debug` |
| --- | --- | --- |
| `#line` directives in emitted C | none | one per function body + per statement |
| Single-file `tur build` C flags | `-O2 ...` | `-g -Og ...` |
| `emit-c` output | unchanged | carries `#line` directives |

### `#line` emission

Spans already round-trip through elaboration onto every `Expr` (Phase 1's
coverage gate guarantees breakpoint-eligible nodes carry a non-zero
`{file, line}`). Phase 4 consumes them at two chokepoints in the emitter:

- **Function entry** (`emit_fn_def`, `src/compiler/emit_fns.c`): right after the
  body's opening `{`, anchoring the frame to the `defn`'s source line.
- **Each statement** (`emit_stmt`, `src/compiler/emit_stmt.c`): at the top of
  the statement dispatch, so native stepping advances line-by-line.

The shared helper `emit_line_directive` (`src/compiler/emit_core.c`) resolves
`span.file_id` to a path via `diag_file_path`, skips a directive that repeats
the previous `(file_id, line)` on the same output stream, and guarantees the
`#` lands at column 0. `emit_line_reset` clears the dedup tracker at each
function-body boundary (bodies may be emitted into a temporary buffer before
being spliced into the file, so the first statement must always re-anchor). The
whole path is gated on the global `g_emit_debug_lines`
(`src/runtime/globals.c`), so it is a single branch -- and a no-op -- when
`--debug` is off.

Macro-expanded code points back to the user's call site, not the macro
definition: substituted forms already carry the call-site span (Phase 1 audit,
"call-site spans survive substitution"), so `e->span` is correct with no
special handling at the emit site.

### Why `-g -Og`, not `-g -O0`

A self-contained single-file `tur build` does **not** link libturi; the
runtime it needs is inlined into the generated C as a preamble. But the stdlib
defns preloaded into every TU include functions (e.g. the `with-contract-handler`
family) whose inline-C references libturi-only symbols
(`tur_set_contract_handler` / `tur_get_contract_handler`) that the preamble does
not define. At `-O2` (and `-Og`/`-O1`) the optimizer's dead-code elimination
drops those unused `static` functions, so the references vanish. At `-O0` they
survive and the link fails with undefined references.

`-Og` is GCC's "optimize for the debugging experience" level: it keeps the DCE
that single-file builds rely on while preserving line-accurate stepping. That
makes it the right floor for `--debug`; `-O0` is not an option here without
also teaching the single-file build to provide those symbols (out of scope).

`TUR_CC_FLAGS` still overrides everything, so a caller who wants `-O0` (in a
context that links libturi, e.g. a directory build) can ask for it explicitly.

## Acceptance test

`tests/run-phase4-gdb.sh` (ctest target `tur_phase4_gdb`) drives the fixture
`tests/fixtures/debugger-phase4/` (a deliberate `abort()` reached through a
`boom <- middle <- main` chain) and asserts:

1. **Gated emission** -- plain `emit-c` carries no `#line`; `--debug emit-c`
   maps the body back to `input.tur`.
2. **Native backtrace** -- `--debug build` produces a binary whose gdb
   backtrace stops on `SIGABRT` and names the Turmeric source (`input.tur:`)
   and Turmeric symbols (`boom` / `middle` / `main`), with no `.c` carrier
   filename.

The gdb half skips cleanly (PASS) when gdb is unavailable, mirroring how the
DAP regression skips without `python3`. The fixture carries
`requires.dedicated-runner` so `tests/run.sh` leaves it to this target. The
crash lives in an inline-C body so it is reliable across environments -- a null
dereference does not fault where page 0 is mapped, but `abort()` always raises
`SIGABRT`.

## Limitations / next

- **Locals render as C carriers.** A struct/option/result local shows its raw
  `int64_t` or tagged-union layout. Phase 5
  ([debugger-native-types-plan.md](./debugger-native-types-plan.md)) adds gdb/
  lldb pretty-printers on top of the C-DWARF the C compiler already emits.
- **Single-file only for the `-g -Og` switch.** The `#line` directives
  themselves flow through every emit path (single-file, multi-file, `--shared`,
  `emit-c`), but the compile-flag adjustment is wired into single-file
  `cmd_build`. Directory/`--shared` builds emit the directives but assemble
  their own compile commands; threading `-g` there is a small follow-up.
- **`lldb`** consumes the same DWARF and works equivalently; only `gdb` is
  exercised by the regression because that is what the CI image ships.
