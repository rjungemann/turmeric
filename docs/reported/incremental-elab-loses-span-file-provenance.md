# Incremental elaboration loses per-file span provenance

**Summary:** under the TR2 incremental elaboration path (now the default for
every `TuriEnv`), spans are attributed to the accumulated `<eval>` source blob
instead of the file the form came from, so `diag_file_path(span.file_id)`
returns nothing usable for anything downstream of elaboration.

**Severity:** medium. No miscompilation, but every consumer that needs to know
*which file* a form came from degrades: the DAP debugger, and diagnostic
locations under `--interpret`.

## Repro

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j
bash tests/run-dap.sh                              # FAIL (before the workaround below)
TUR_NO_INCREMENTAL_ELAB=1 bash tests/run-dap.sh    # PASS, 20/20
```

Two symptoms in one transcript:

```
FRAME out #0 main ?:19            <- expected `input.tur:19`; no `source` object
FAIL: timeout waiting for event stopped   <- the line-13 conditional bp never fires
```

The diagnostic surface shows the same thing directly -- a `--interpret` error
that the whole-program path reports against the real file comes back as
`<eval>:64:3` (or `<unknown>:56:6`) on the incremental path.

## Root cause

`diag_file_path` (src/compiler/diag.c:90) resolves a `file_id` through the
`files_` registry and returns NULL when the id has no registered `SourceFile`.
The incremental session concatenates the accumulated prefix and the new tail
into `env->src_combined` and elaborates that blob, so the spans it produces
carry the blob's file id, not the id registered for `input.tur`.

Both DAP failures follow from that one fact:

- `dap_stack_trace` (src/turi/dap.c:391) emits a `"source"` object only when
  `fr.file_path[0]` is non-empty; `turi_debug_frame_at` fills `file_path` from
  `diag_file_path(s.file_id)` (src/turi/eval.c:11195).
- `setBreakpoints` stores a basename (`DapBp.file`) and the pause hook matches
  it against each node's source file, which resolves through the same call.

## Fix directions

The real fix is to keep per-file provenance across the incremental blob --
either by registering the originating file for each spliced region and
remapping span file ids as the tail is appended, or by giving the session a
span-offset table it can invert. Neither is small.

Note the macro-visibility rules already carry two lookup-time workarounds for
the same "the incremental path moves the stdlib/user boundary" problem
(`elab_lookup_macro` in src/compiler/elab_core.c, and now `elab_lookup_sym` in
src/compiler/elab_module.c). Span provenance is the third instance; a general
fix would retire all three.

## Current workaround

`cmd_eval_h` opts a debug session out of incremental elaboration
(`turi_env_set_incremental_elab(env, false)` when `debug` is set, src/main.c).
A debug session loads one program once, so the incremental win -- amortising a
growing prefix across many REPL turns -- does not apply, and opting out costs
nothing. Diagnostic locations under plain `--interpret` are still affected.
