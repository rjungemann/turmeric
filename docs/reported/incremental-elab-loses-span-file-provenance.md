# Incremental elaboration loses per-file span provenance

**Summary:** under the TR2 incremental elaboration path (now the default for
every `TuriEnv`), spans are attributed to the accumulated `<eval>` source blob
instead of the file the form came from, so `diag_file_path(span.file_id)`
returns nothing usable for anything downstream of elaboration.

**Severity:** medium. No miscompilation, but every consumer that needs to know
*which file* a form came from degrades: the DAP debugger, and diagnostic
locations under `--interpret`.

**Status (2026-07-29): partially fixed. The report conflated two different
bugs.**

- **FIXED -- `--interpret` diagnostic locations.** This half was never about
  incremental elaboration at all; see
  [The `--interpret` half was not incremental](#the---interpret-half-was-not-incremental).
- **STILL OPEN -- DAP under incremental elaboration.** This half is genuine and
  the `cmd_eval_h` workaround is still load-bearing; see
  [The DAP half is genuinely incremental](#the-dap-half-is-genuinely-incremental).

## The `--interpret` half was not incremental

The report attributes the `<eval>:64:3` diagnostic locations to the incremental
path. They reproduce **identically** with the incremental path disabled:

```sh
$ tur --interpret sp.tur
<eval>:64:10: error [TUR-E0001]: '+' arg 2: type mismatch - expected int, got cstr
$ TUR_NO_INCREMENTAL_ELAB=1 tur --interpret sp.tur
<eval>:64:10: error [TUR-E0001]: '+' arg 2: type mismatch - expected int, got cstr   # same
```

The cause is the eval-blob concatenation itself (`turi_eval_file` -> `turi_eval`
appends the file to the accumulated prelude and registers the whole blob as one
synthetic `<eval>` SourceFile), which happens on **both** paths. Incremental
elaboration was a red herring here.

The fix needed no span remapping, because the tree already had a route that
keeps provenance -- the one the debugger was using. `cmd_eval_h` brought the
user file in via `(load "path")` under `--debug` precisely so "a loaded file
gets its own file_id and keeps its real path + 1-based line numbers", while
plain `--interpret` went through the blob. Both now take the `(load ...)` route:

```
tests/fixtures/errors/interpret-diag-names-real-file/input.tur:15:10: error [TUR-E0001]: ...
```

### It was blocked by a second, unrelated bug

Switching the entry file onto the `(load ...)` route initially broke 16
interpreter fixtures, all in one family (`sweet-exp-*`, `lang-*`, `neoteric`,
`curly-infix`, ...). Root cause: **the load splicer picked a loaded file's
dialect from its file EXTENSION only and ignored an inline `#lang` directive.**
That is an independent defect, reproducible with no reference to `--interpret`:

```turmeric
;; sweetlib.tur -- first line is `#lang turmeric/sweet`, extension is plain .tur
;; uses.tur:
(load "sweetlib.tur")
;; => sweetlib.tur:1:1: error: unexpected character '#' (0x23)
```

A `.tur.sweet` extension worked; an inline directive did not. Fixed in
`load_expand_forms` (`src/compiler/elab_toplevel.c`) by running the same
`detect_lang_layered` sweep the entry file gets in `resolve_reader_type`
(`src/main.c`), so the directive is both honoured and stripped -- extension
still wins for the base when it selected a non-default reader, layers ride
along either way. Fixture:
`tests/fixtures/load-inline-lang-sweet/`.

With that in place the `--interpret` route change is clean: `tests/run.sh`
2411 passed / 0 failed, `tests/run-turi.sh` 1667 passed / 0 failed, plus
run-dap / run-debugger / run-flags / run-cli / run-offtree-load /
run-parse-check all green.

## The DAP half is genuinely incremental

This half of the report stands exactly as filed, and the `cmd_eval_h` workaround
(`turi_env_set_incremental_elab(env, false)` when `debug` is set) is still
required. Removing it -- **even now that both paths go through `(load ...)`** --
reproduces the reported symptom verbatim:

```
FRAME out #0 main ?:19
FAIL: timeout waiting for event stopped
```

So the incremental path drops per-file provenance for forms that were spliced in
by `load`, independently of how the entry file is ingested. That is the bug the
"Root cause" and "Fix directions" sections below describe, and it is the one
still needing the region-remap work. Everything below this line is the report as
originally filed.

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
(Superseded 2026-07-29: the `--interpret` sentence no longer holds -- see
[The `--interpret` half was not incremental](#the---interpret-half-was-not-incremental).
The workaround itself is still required.)
