# REPL `:explain` + Try-Turmeric Meta-Commands Plan

Status: Piece 1 (Slice A) COMPLETED. Piece 2 (Slice B) Pending.

Two small, related REPL improvements:

1. Add `:explain` to the native REPL so users can ask the running session to
   expand on the diagnostic code attached to the most recent error / warning
   (or an explicit `TUR-E####` code).
2. Make the existing REPL meta-commands (`:help`, `:doc`, `:type`,
   `:reload`, `:run`, `:reset`, plus the new `:explain`) work in the
   Try-Turmeric web console. They currently fall through to the WASM
   evaluator, which sees `(:doc println)` as "call a `Sym` as a function"
   (TUR-E0003 / "not callable") -- see the report at the head of this plan.

Both pieces are user-facing polish, not codegen or type-system work. Scope
is intentionally tight: no new diagnostic copy, no new doc DB, no new
backend entry points beyond what each piece literally needs.

## Background

- The native REPL meta-command dispatcher lives in
  `src/turi/repl.c` (see `k_meta_cmds[]` at `src/turi/repl.c:90` and the
  single-line meta-command parser at `src/turi/repl.c:900`). Recognised
  commands: `:help`, `:quit`/`:q`, `:type`, `:doc`, `:reload`, `:run`,
  `:reset`.
- `tur explain TUR-E####` already exists at `src/main.c:12122` and is
  backed by `diag_explain()` / `diag_code_from_string()` in
  `src/compiler/diag.c` (lookup table at `src/compiler/diag.c:264`,
  long-form text starting around `src/compiler/diag.c:379`). The
  explanation strings are written to a `FILE *`.
- The web REPL forwards each submitted line to the WASM compiler via
  `executeCode(code, ...)` in `web/main.js:1919` (called from the
  `initReplInput` keydown handler). Nothing pre-screens for `:`-prefixed
  meta-commands, so they hit the compiler as ordinary source.
- The WASM build exports `turi_doc_lookup` (`src/web/wasm_glue.c:353`,
  declared in `src/web/wasm_glue.h:152`); no analogous bridge exists for
  `:type`, `:explain`, or `:help` yet.

## Goal

### Piece 1 -- `:explain` in the native REPL

`:explain` should answer two questions:

1. "Tell me more about *that* error/warning I just saw."
   - Form: bare `:explain` (no argument).
   - Behaviour: look up the diagnostic code from the most recent
     `turi_eval` / `turi_eval_typed` result and call `diag_explain()`.
   - If the last evaluation succeeded or produced no coded diagnostic,
     print: `:explain -- no recent diagnostic to explain. Try :explain
     TUR-E#### for a specific code.`
2. "Tell me about this code."
   - Form: `:explain TUR-E0001` (or `tur-e0001`, normalised to upper case
     for the lookup).
   - Behaviour: same as `tur explain TUR-E0001` -- delegate to
     `diag_explain()`. On miss: `unknown diagnostic code 'TUR-E9999'`.

This intentionally avoids re-compiling a snippet (the legacy `tur
explain "<code>"` path); the REPL already has a freshly-failed
elaboration available via its capture buffer, and a snippet form
duplicates `:type`.

### Piece 2 -- Meta-commands in Try Turmeric

The web REPL should intercept the same meta-commands the native REPL
does, *before* shipping the line to the WASM evaluator, and dispatch
them to the appropriate JS-side handler (which may in turn call into
WASM exports). Concretely it needs to handle: `:help`, `:doc <sym>`,
`:type <expr>`, `:explain [code]`, `:reset`. `:reload` and `:run` are
deferred (they map to filesystem state Try Turmeric does not own in the
same way -- see "Out of scope" below).

## Plan

### Slice A -- `:explain` in `src/turi/repl.c` [COMPLETED]

A1. Add a "last diagnostic code" slot.

  - The REPL already shares a `TuriEnv` across lines (see
    `src/turi/repl.c:218` for how `:type` reuses it). Add a small
    capture state next to the REPL state:
    ```c
    static char g_last_diag_code[16] = "";  /* "" = no recent code */
    ```
  - `turi_eval_typed` already accepts a stderr-capture callback (per
    `src/turi/eval.h:341` and `:343`); the REPL eval loop should pass a
    callback that records the *first* `TUR-E####` / `TUR-W####` code it
    sees per evaluation, clearing the slot at the start of each line.
  - On successful evaluation with no warnings, the slot ends up empty
    (correct -- nothing to explain).

A2. Extend `k_meta_cmds[]` (`src/turi/repl.c:90`) with `":explain"`.

A3. Extend the help banner at `src/turi/repl.c:516` with the one-liner:
  ```
    :explain [code]     explain the most recent error, or a TUR-E#### code
  ```

A4. Add the dispatch arm next to `":doc"` (`src/turi/repl.c:932`):
  - Bare `:explain` -> if `g_last_diag_code[0]` is set, call
    `diag_explain(diag_code_from_string(g_last_diag_code), stdout)`,
    else print the no-recent-diagnostic line.
  - `:explain ARG` -> normalise `ARG` to upper case, validate with
    `looks_like_diag_code_()` (today static in `src/main.c:12016`; lift
    it to `src/compiler/diag.h` as `diag_looks_like_code()` so both the
    CLI and the REPL share one validator). On hit, call `diag_explain`;
    on miss, print `unknown diagnostic code 'ARG'`.

A5. Tests:
  - One new fixture exercising `tur repl --eval ":doc cons" --eval ":explain"`
    style (use whatever scripted-REPL harness `tests/fixtures/repl-*/`
    already uses; reuse `requires.no-leak-check` only if existing REPL
    fixtures need it).
  - Two cases:
    1. Trigger a `TUR-E0003` (unbound symbol), then `:explain` shows the
       canned text.
    2. `:explain TUR-E0001` from a fresh session prints the type-mismatch
       explanation.

### Slice B -- Web REPL meta-commands

B1. Add a `:`-prefix interceptor in `initReplInput` (`web/main.js:1919`):
  - Refactor the keydown body so that, after the trimmed `code` is in
    hand, a `dispatchReplMetaCommand(code)` call gets first crack. If it
    returns "handled", echo the input line to the console (the way
    `executeCode` would have) and skip the normal eval path. Otherwise
    fall through to `executeCode` as today.

B2. Implement `dispatchReplMetaCommand(line)` (new function next to
   `initReplInput`):
  - Split on first whitespace -> `cmd`, `arg`.
  - Recognised commands and their JS behaviour:
    - `:help` -- print a hard-coded help block matching the native
      REPL's banner, trimmed to web-relevant commands (no `:reload`,
      no `:run`, no `:quit`).
    - `:doc SYM` -- call the existing `Module.ccall('turi_doc_lookup',
      'string', ['string'], [SYM])`; if it returns null/empty, print
      `no documentation for 'SYM'`.
    - `:type EXPR` -- need a new WASM export. See B3.
    - `:explain [CODE]` -- need a new WASM export. See B3.
    - `:reset` -- call the existing `Module.ccall('turi_wasm_reset', ...)`
      (already exported per `src/CMakeLists.txt:788`); also clear the
      console pane the way the Clear button does.
  - Unknown `:foo` -- print `unknown meta-command ':foo' -- try :help`
    (do *not* forward it to the evaluator; that is what produces the
    confusing `Sym is not callable` error today).

B3. New WASM exports (`src/web/wasm_glue.c`, sibling to
   `turi_doc_lookup` at `src/web/wasm_glue.c:351`):
  - `const char *turi_type_of(const char *expr)` -- elaborate `expr`
    against the shared `g_env` using the existing `turi_eval_typed`
    type-tag machinery (the `:type` native handler at
    `src/turi/repl.c:246` already shows the recipe) but return the
    type tag string instead of evaluating. On error, return the
    formatted diagnostic line. The returned `char *` follows the same
    static-buffer convention as `turi_doc_lookup`.
  - `const char *turi_explain(const char *code_or_null)` -- if `code` is
    null/empty, look up the last-diagnostic slot added in Slice A1; else
    validate + look up `code`. Write the `diag_explain` output into a
    `static char[]` via `open_memstream`/`fmemopen` (whichever the
    Emscripten build already uses elsewhere; if neither, allocate a
    `FILE*` via `tmpfile()` and read it back -- the buffers are small).
  - Add both names to the `-sEXPORTED_FUNCTIONS=...` list at
    `src/CMakeLists.txt:788`.

B4. Tests:
  - Extend the Playwright suite under `web/tests/` with a spec that:
    1. Submits `:help` -> asserts the console contains the help banner.
    2. Submits `:doc cons` -> asserts the console contains the
       known-good doc snippet.
    3. Submits code that fails (e.g. `(foo)`) then `:explain` -> asserts
       the canned TUR-E0003 text appears.
    4. Submits `:type 1` -> asserts the console contains `int`.
    5. Submits `:nonsense` -> asserts `unknown meta-command`.

## Out of scope

- `:reload` and `:run`: Try Turmeric's filesystem is the tab list and
  ZIP-loaded project, not a real FS. A separate plan can map these to
  "reload current tab" / "run named tab"; not in this slice.
- New diagnostic explanation copy. Several diagnostic codes return a
  short / generic explanation today; *improving* that copy is its own
  conv-style track. This plan ships whatever `diag_explain` already
  produces.
- A snippet form of `:explain` (e.g. `:explain (foo)`). `:type` already
  covers "compile this and tell me what happened"; `:explain` here
  stays focused on diagnostic codes.
- Auto-suggesting `:explain` in the error footer. Worth doing but
  belongs in a follow-up once both pieces are in place.

## Risks / open questions

- The "last diagnostic code" capture in Slice A1 needs to survive an
  evaluation that emits multiple diagnostics. Recording the **first**
  coded diagnostic per line is the simplest rule and matches what the
  user usually wants (the cause, not the cascade); call it out in the
  help text.
- `turi_type_of` and `turi_explain` need to be added to
  `EXPORTED_FUNCTIONS` *and* the WASM rebuild has to ship to
  `web/turmeric.js` before the Playwright tests will pass; coordinate
  with the `tur run wasm` step in the PR.
- `diag_looks_like_code()` lift to `diag.h` is a tiny refactor but
  touches an already-mixed file; keep it to a one-function move with no
  behaviour change.

## Done when

- `bash tests/run.sh` (10-minute timeout) is green with the new REPL
  fixture(s).
- `tur repl` accepts `:explain` per Slice A.
- The web REPL handles the five Slice B commands locally and never
  forwards a `:`-prefixed line to the evaluator.
- The Playwright spec passes against a freshly built `web/turmeric.js`.
