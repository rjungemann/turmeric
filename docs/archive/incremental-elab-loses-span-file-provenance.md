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

## RESOLVED 2026-08-13 -- the DAP half

The remaining half is fixed, the `cmd_eval_h` workaround is removed, and debug
sessions keep the incremental path.

### It was the file REGISTRY, not the spans

The root-cause section above says the incremental session "elaborates that blob,
so the spans it produces carry the blob's file id, not the id registered for
`input.tur`". That is not what happens. Instrumenting `diag_register_file` /
`diag_reset` / `diag_file_path` across a DAP session shows the spans are fine and
the registry is not:

```
[files] RESET (dropping 41)
[files] register id=0 path=<eval>
[files] MISS id=40 (count=1)        <- x18, every frame and breakpoint query
```

Turn 1 registers 41 files -- the blob plus 40 `(load ...)`ed ones, with the
user's `input.tur` last at id 40. `diag_reset()` then clears the whole table
every turn (`files_[i] = NULL`, `file_count_ = 0`). Turn 2 re-registers **only**
id 0, because the incremental path reuses the Forms it already parsed and so
never re-runs their load splices -- while those Forms still carry id 40.

So the spans keep correct provenance throughout. The id they name simply stops
resolving. That is why the symptom is `?:19` (a frame with no path) rather than
a frame attributed to `<eval>`.

This also explains why the report's two halves behaved so differently, and why
the `--interpret` half turned out not to be incremental at all: that one was
about which file the source *went into*, this one is about whether the registry
still knows the file at lookup time.

### Fix

`diag_files_save` / `diag_files_restore` (`src/compiler/diag.c`), called around
the `diag_reset()` in `turi_eval_impl`: snapshot the table, reset, register this
turn's blob at id 0, then re-register every saved entry from id 1 up that the
new turn has not already claimed. Id 0 is deliberately skipped -- the saved
entry there is the *previous* blob and would clobber the current one.

Sound because the interpreter retains its eval arenas for the life of the env,
so the saved `SourceFile` pointers stay valid across turns. `diag_reset()`
itself is unchanged, because clearing the registry is correct for a compiler
driver looping over files whose per-file arenas are about to go away; the header
says so at the new declarations, so the next reader does not "simplify" this by
making `diag_reset()` keep files globally.

This is much smaller than the report's estimate ("either by registering the
originating file for each spliced region and remapping span file ids ... or by
giving the session a span-offset table it can invert. Neither is small.") -- no
remapping and no offset table, because no span ever moved.

### The workaround is gone

`cmd_eval_h`'s `turi_env_set_incremental_elab(env, false)` for debug sessions is
removed; the comment there now records what the cause actually was. Debug
sessions get the incremental path like everything else.

### `tests/run-dap.sh` is now a real regression guard

It was not one before: with the workaround in place it passed whether or not
this bug existed. Verified both directions -- with the workaround removed and
the fix reverted it reproduces the reported transcript verbatim
(`FRAME out #0 main ?:19` then `FAIL: timeout waiting for event stopped`), and
with the fix it is 20/20.

### Does this retire the other two workarounds?

The report notes two sibling lookup-time workarounds for "the incremental path
moves the stdlib/user boundary" (`elab_lookup_macro` in `elab_core.c`,
`elab_lookup_sym` in `elab_module.c`) and suggests a general fix would retire all
three. It does not: those are about *name visibility* across the moved boundary,
whereas this was the diagnostic file registry being cleared out from under
reused Forms. Same incremental path, unrelated mechanisms. They stay.

### Verification

`tests/run-dap.sh` 20/20. `tests/run-debugger.sh` 17/17. `tests/run.sh` 2596
passed, 0 failed. `tests/run-turi.sh` 1782 passed, 0 failed.
`tests/run-flags.sh` 78 passed, 0 failed. `tests/run-offtree-load.sh` 3 passed,
0 failed.
