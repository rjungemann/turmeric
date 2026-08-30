# Editor intelligence follow-through: scope, rename, and a time-travel tracer

> **Status: Executed (2026-08-30)** -- S1, A1, A2, A3, A6, W1-W4, T1 and T2
> landed. T3 (the Try Turmeric timeline) is deferred, as the plan itself
> phases it. The Playwright half of track W could not be run in the execution
> environment (no `emcc` to build `web/turmeric.js`); see
> [§9 Execution record](#9-execution-record).
> Written 2026-08-29 against v0.41.0.
> **Type:** LSP / interpreter / web client.
> **Source:** the follow-up sections of c2mp's
> `~/Projects/c2mir-playground/c2mp/docs/lsp-plan.md` (S11 occurrences and
> scope, S12 the prompt and the transcript, S13 commands, headers and files)
> and its `docs/debugger-plan.md` (a recording debugger, built), plus rename,
> which c2mp lists as not covered in both S11.4 and S13.4.
> **Prior art, in the other direction:** c2mp's plan borrowed its phasing from
> [`try-turmeric-lsp-plan.md`](../archive/try-turmeric-lsp-plan.md). This plan
> borrows the parts back. Where c2mp had to build a language server, a scope
> analysis and every widget by hand, Turmeric already owns the elaborated tree,
> a full LSP, a DAP server and Monaco -- so most of what follows is smaller
> here than it was there, and two items are smaller by an order of magnitude.
> **Depends on:** `src/lsp/lsp_collect.c` (global symbol harvest),
> `src/lsp/lsp_util.c:251` (`lsp_scan_occurrences`), `src/turi/eval.h` (the
> Phase 3 debugger control API), `src/turi/dap.c`, `web/lsp-client.js`,
> `web/main.js:2790` (`initReplInput`).

## 0. Summary

Three tracks, in dependency order:

| | What | Where it lands |
|---|---|---|
| **S** | A lexical scope resolver over the elaborated tree -- the one thing every item below is waiting on | `src/lsp/lsp_scope.c` (new) |
| **A** | LSP: scope-aware highlight, `textDocument/rename`, `textDocument/references` | `src/lsp/lsp.c`, every client for free |
| **W** | Try Turmeric: prompt completion / hover / signature help, console hover, meta-command completion | `web/main.js`, `web/lsp-client.js` |
| **T** | A time-travel tracer for the interpreter: record a run, scrub it, step backwards | `src/turi/trace.c` (new), `src/turi/dap.c`, `tur trace` |

S is the keystone. **Turmeric's symbol index knows only global bindings**
(`lsp_collect.h`: "records every global binding"), so a parameter named `x`
today highlights every `x` in the file, completion never offers a `let` name,
and rename cannot be written safely at all -- which is exactly the reason c2mp
gives for not having shipped rename (S11.4: "the shadowing caveat is tolerable
for a highlight and is not tolerable for an edit that rewrites text").

T is independent of S and can be built in parallel by a second pair of hands.

### 0.1 What is already true, so the plan does not rebuild it

Worth stating, because half of c2mp's follow-up work is machinery Turmeric
already has:

- `lsp_scan_occurrences` (`src/lsp/lsp_util.c:251`) already skips `;` line
  comments, string literals, `#| |#` block comments **and** inline-C bodies,
  and already matches whole identifiers only. c2mp's S11.2 is the same idea and
  skips one fewer region.
- `textDocument/documentHighlight` already ships (`src/lsp/lsp.c:1048`) and
  already distinguishes the definition (kind 3) from uses (kind 1).
- Symbols already carry `file_path` (`src/lsp/lsp_sym.h`), `documentSymbol`
  already filters on it (`lsp.c` -- "only emit symbols defined in this file"),
  and completion already ranks document-local symbols ahead of imported ones
  (`lsp.c:1637`). c2mp's S13.3 "file identity, and what it unlocked" is
  largely already the case here; A6 is a verification pass, not a build.
- The DAP server already does breakpoints, conditional breakpoints, stepping,
  `stackTrace`, `scopes`, `variables` and in-frame `evaluate`
  (`src/turi/dap.c:656`). The tracer adds a time axis to a debugger that
  exists, rather than being one.

---

## 1. S -- lexical scope, the keystone

### 1.1 What is missing and why it blocks three features

`lsp_collect_program` walks an `EX_PROGRAM` and records global bindings into
`LspSymbol[]`. There is no record of `defn` parameters, `let` / `loop`
bindings, `fn` parameters, `for` binders, or pattern binders, and no span for
the region a binding is visible in. Every consumer therefore answers a
*textual* question when it was asked a *lexical* one:

- **Highlight**: `on_document_highlight` looks the name up in the global index
  for the definition marker and then scans the whole buffer. A local's uses in
  one function come back alongside every unrelated `x` in the file.
- **Completion**: `on_completion` (`lsp.c:1567`) offers globals, stdlib and
  keywords. The parameter the cursor is standing inside is not offered.
- **Rename**: cannot be written. Renaming `total` when a `let` shadows a global
  `total` -- or vice versa -- silently changes what the program means.

### 1.2 Design

A new pass, `lsp_scope.c`, collected the same way symbols are: bracketed by
`lsp_scope_begin()` / `lsp_scope_end()`, fed from the same elaboration
`lsp_collect` already hooks, so **no second traversal and no second front end**
(the reason `lsp_collect.c` was split out of `main.c` in the first place --
`src/lsp/lsp_collect.h` says so -- applies here identically).

```c
/* One lexical binding: a name, where it was bound, and the source region it
 * is visible in. Locals only; globals stay in LspSymbol. */
typedef struct {
    char       name[128];
    char       type_str[256];   /* rendered type, or "" if not inferred */
    LspBindKind kind;           /* param, let, loop, fn-param, for, pattern */
    uint32_t   def_line, def_col_start, def_col_end;   /* 1-based */
    uint32_t   scope_start_off, scope_end_off;         /* byte offsets */
    int        depth;           /* nesting depth, for shadow resolution */
} LspBinding;
```

Two rules decided by c2mp's bugs rather than rediscovered:

- **A binding's scope starts at its binder, not at the body.** c2mp's S11.3
  found a parameter that was not in scope at its own declaration, because the
  scope began at the body's `{`; the caret on the parameter name then resolved
  to whatever global shared it and highlighted the whole file. In Turmeric the
  equivalent is starting a `defn` parameter's scope at the body rather than at
  the `[` of the parameter vector, and a `let` binding's at the body rather
  than at its own value form. Start at the binder vector.
- **Innermost wins, and the answer is one binding.** `lsp_scope_lookup_at(off,
  name)` returns the innermost `LspBinding` whose scope covers `off`, or NULL
  meaning "this is the global". Everything downstream branches on NULL.

`LspBindKind` is a real enum for the same reason `LspSymKind` is one
(`lsp_sym.h`): the LSP wire enums for completion and for symbols number the
same distinctions differently, and one neutral tag maps to both.

### 1.3 Cost

The walk is over an already-elaborated tree and only runs when a collection is
active (`lsp_collect_active()` gates it), so the compile path pays nothing.
Storage is per-document and freed with the document. Cap the array like
`LspSymbol` is capped and record the truncation rather than silently
degrading -- a rename that saw a truncated binding table must refuse, not
guess (see 2.2).

### 1.4 What S deliberately does not do

- **Macro-introduced bindings.** A binding a `defmacro` expands into has a span
  in expanded source that does not exist in the file. It is recorded with
  `def_line == 0`, which every consumer reads as "not renameable, not
  highlightable" rather than as an offset.
- **Typeclass method dispatch.** Resolving which `definstance` a call lands in
  is a different question and no feature below asks it.

---

## 2. A -- the LSP features

### 2.1 A1: scope-aware `documentHighlight`

`on_document_highlight` gains one branch before the scan:

```
binding = lsp_scope_lookup_at(offset_of_cursor, name)
if (binding)  scan only [binding->scope_start_off, binding->scope_end_off)
              and mark binding->def_* as kind 3 (Write)
else          scan the whole buffer, as today
```

`lsp_scan_occurrences` grows an offset range rather than a second
implementation; it already advances a line counter through skipped regions
(`lsp_util.c:267`, `ADVANCE`), so a range that starts mid-file still reports
honest line numbers as long as the scan *begins* at byte zero and only starts
*emitting* at `scope_start_off`. Do it that way; starting the scan itself at an
offset would put the comment/string state machine in the wrong state.

Turmeric's version is stronger than c2mp's here for free: the minimap paints
highlights down the whole file (`lsp.c:1014` records that this is why the
provider exists at all), so narrowing a local to its function is visible at a
glance rather than only under the cursor.

**Known limit, stated in the code:** a global shadowed by a local of the same
name has the local's uses reported as its own only if the resolver is wrong;
with S in place it is correct, which is what makes 2.2 possible.

### 2.2 A2: `textDocument/rename`

The feature the whole plan is pointed at. Advertise
`"renameProvider":{"prepareProvider":true}` in the capabilities block
(`lsp.c:710`) and add two handlers.

**`textDocument/prepareRename`** is the honesty valve, and it is why the
`prepareProvider` form is used rather than the bare boolean. It answers with
the range that would be renamed, or with an error message the editor shows
before the user types a new name. It must refuse, with a reason, when:

| Situation | Message |
|---|---|
| The cursor is not on an identifier | (null -- no rename here) |
| The name resolves to a macro-introduced binding (`def_line == 0`) | `cannot rename a macro-introduced binding` |
| The binding table for this document was truncated | `file too large to rename safely` |
| The name is a stdlib symbol not defined in this workspace | `cannot rename stdlib symbol` |
| The name is exported by this spice's `build.tur` `:exports` and R2 is not enabled | `renaming an exported symbol needs --rename-exports` |

Refusing is a feature. c2mp's S13.4 lists rename as not covered specifically
because the safe half was missing; shipping a rename that quietly corrupts a
shadowed binding would be worse than not shipping one.

**`textDocument/rename`** returns a `WorkspaceEdit`. Two phases:

- **R1 -- local and single-document.** The binding resolves to an `LspBinding`:
  edits are exactly the occurrences within its scope range, which is A1's
  answer reused. Or it resolves to a global defined in *this* document and used
  in *this* document: edits are the whole-buffer occurrences. One `changes`
  entry, one URI. This is the common case and it is completely safe, because
  the scope resolver bounds it.
- **R2 -- across the workspace.** A top-level `defn` / `def` / `defstruct` used
  by sibling modules. The file set comes from the manifest, not from the open
  documents: walk-up to `build.tur`, take the project's own `src/` plus each
  `:spices` dep with `:path` (the same resolution `tur check` already does per
  file -- see CLAUDE.md, "Per-file Commands Inside a Spice"), and scan each file
  for occurrences, skipping any file that does not `import` the defining module.
  Emit one `changes` entry per file that has a hit.

  **Do the include/import resolution in C, not in the client.** This is c2mp's
  S13.2 result and the reasoning transfers exactly: a second implementation of
  the module search path in JavaScript will eventually disagree with the
  compiler's, and a disagreement here does not produce a cosmetic bug, it
  produces an edit applied to the wrong file.

  R2 is gated on the symbol not crossing a `:url`-backed dependency boundary:
  a name that another *fetched* spice imports cannot be renamed from here, and
  prepareRename says so.

**Shadowing is the test suite, not a footnote.** Fixtures that must pass before
R1 is called done: a `let` shadowing a global of the same name (rename the
inner one, the outer is untouched, and vice versa); a parameter shadowing a
`defn`; two sibling `let` forms binding the same name in disjoint scopes
(renaming one leaves the other alone); a name appearing inside a string, a
comment and an inline-C body (untouched -- already guaranteed by the scanner,
now load-bearing); and the same set again in a `.tur.sweet` file, because
sweet-exp and neoteric spell a call `f(x)` and the scanner is byte-oriented,
so `f` in `f(x)` must still be a whole-identifier hit.

### 2.3 A3: `textDocument/references`

Falls out of A1 and R2 together and is worth advertising once they exist:
references is R2's file walk without the edit. Add it in the same PR as R2 --
implementing the workspace scan and then not exposing it as references would be
leaving the cheapest feature in the plan on the floor.

`includeDeclaration` is honored: the definition is a reference when the client
asks for it, and is `kind 3` in highlight terms.

### 2.4 A6: cross-file identity -- verify, then close two gaps

c2mp's S13.3 landed file identity on symbols; Turmeric has had it since
`LspSymbol` gained `file_path`. Two of the four things c2mp lists as unlocked
should be *checked* rather than assumed here, and two are open:

- **Verify:** go-to-definition on a symbol defined in another module of the
  same spice lands in that module's file (should already work through
  `file_path` + `lsp_path_to_uri`), and completion offers names from imported
  modules (`lsp.c:1637`'s two-pass local/non-local ranking implies it does).
- **Close:** *a definition outranks a declaration* has no Turmeric analogue
  (there are no headers), but the equivalent -- a re-exported name resolving to
  the re-export rather than to the defining `defn` -- has the same symptom, and
  should be checked with a fixture.
- **Close:** in Try Turmeric, go-to-definition across tabs must **switch tabs
  first**. c2mp's S13.3 note is exactly right about why: a span indexes its own
  file, so selecting that offset in whatever tab happens to be open is worse
  than refusing. `web/main.js` owns the tab list (`tabs`, `web/main.js:118`)
  and the client already receives it (`getTabs`, `web/main.js:570`), so this is
  a client change, not a server one.

---

## 3. W -- Try Turmeric: the prompt and the transcript

Everything the LSP does today reaches the Monaco editor and nothing reaches the
`turi>` prompt. `initReplInput` (`web/main.js:2790`) is a bare `<input>` with
Enter-to-submit and ArrowUp/ArrowDown history, and `#repl-input`
(`web/try/index.html:308`) has no highlight layer behind it -- which is
precisely the blocker c2mp recorded twice and then found to be much smaller
than it looked.

### 3.1 W1: completion, hover and signature help at the prompt

c2mp's S12.1 is the load-bearing finding and it transfers: **the blocker was
two functions, not the widget stack.** Here it is even smaller, because Monaco
providers are already registered and the LSP already answers -- what is missing
is a caret position to ask about and a surface to draw on.

The recommendation is to **not** hand-build a completion widget over the
`<input>`. Replace `#repl-input` with a **single-line Monaco editor**
(`monaco.editor.create` with `lineNumbers: 'off'`, `glyphMargin: false`,
`folding: false`, `scrollBeyondLastLine: false`, `wordWrap: 'off'`, fixed
height, and `Enter` rebound). Monaco supports this configuration explicitly, it
is already in the bundle, and it means completion, hover and signature help at
the prompt are *the providers that already exist* rather than a second copy of
the state machine.

That reduces c2mp's S12 to its two genuinely hard parts, both of which survive
the change of substrate:

- **Key reconciliation (S12.3).** `Enter` submits, `ArrowUp`/`ArrowDown` walk
  history, and the completion list wants all three. Do it as c2mp did: **one
  handler, not two listeners**, because with two the behavior depends on
  registration order, which a reader has to infer rather than read. In Monaco
  terms: a single `onKeyDown` that consults
  `editor.getContribution('editor.contrib.suggestController')` for whether the
  suggest widget is open, and only then yields the key.
- **`Escape` must `stopPropagation`.** Try Turmeric has document-level Escape
  handlers; dismissing a popup must not also stop a running program. c2mp
  verified this by starting an infinite loop, opening the list, and pressing
  Escape. Copy the test.

Also copy the bug c2mp found while writing the history test (S12.4), because
the same shape exists here: `setBusy(true)` disables the prompt, **disabling a
focused element blurs it**, and nothing puts focus back
(`web/main.js:1153` re-enables the input but does not refocus it). Fix:
`setBusy` remembers whether the prompt is what lost focus and restores it only
in that case, or it steals focus from wherever the user deliberately went.

### 3.2 W2: the prompt may only offer what the session can evaluate

The rule from c2mp's S12.2, and the reason W1 is not simply "point the existing
providers at the new editor":

> An offered name that the session cannot resolve is not merely unhelpful --
> accepting it produces an expression that fails to evaluate.

The Try Turmeric prompt evaluates against the wasm interpreter session, whose
visible names are the prelude, the stdlib, and whatever previous prompt lines
and Runs have defined. The Monaco editor's index is built from the **active
tab**, which is a different set: a `defn` typed in a tab that has never been
Run is not callable at the prompt.

So the prompt gets its own document identity -- a synthetic URI such as
`file:///<session>/repl.tur` opened against the LSP session and kept in sync
with what the interpreter has actually accepted -- rather than sharing the
active tab's. Completion at the prompt then answers from that document plus
stdlib, which is the truthful set.

Note what this needs from the server: nothing. `lsp_session_handle`
(`src/lsp/lsp_session.h`) is already document-keyed and already holds several
documents at once.

### 3.3 W3: hover in the transcript

c2mp's S12.5 is the one item that needs no markup at all, and the reasoning
holds here byte for byte: `appendToConsole` (`web/main.js:919`) inserts HTML it
built itself, and console lines are ordinary text nodes, so
`caretPositionFromPoint` lands on the right node without wrapping a single
identifier in a span. Wrapping them would put an escaping surface exactly where
"interpreted stdout stays inert" lives.

**Which lines answer is a judgement, not a capability.** The echoed `turi> ...`
lines and error lines do; program stdout does not. Text a program happened to
print is not a symbol reference, and a hover card about `println` over a program
that printed the word "println" is a lie about what that text is.

The most valuable case is the same one c2mp found: a diagnostic naming a symbol
the index knows. Hovering the name in `unknown symbol: str-concat` answers the
question the message raises.

### 3.4 W4: meta-command completion

Try Turmeric has `:help`, `:doc`, `:type`, `:explain` and friends, dispatched
by `dispatchReplMetaCommand` and described by a hand-written help text
(`web/tests/meta-commands.spec.js` asserts on both). c2mp's S13.1 fires
completion on a `:` **at the start of a line and nowhere else**, which matters
in Turmeric too: `:foo` mid-expression is a keyword literal and a map key
(`#map{:name name}`), and offering the REPL's vocabulary there would be noise.

Move the command list into one table that drives three consumers -- the
dispatch switch, the generated `:help` text (aligned on the widest label, so
adding a command cannot leave the column crooked), and the completion list. It
is about to exist in three places, which is how the help text and the switch
quietly stop agreeing.

Second context: `:doc ` and `:type ` take a symbol, so completion after them
offers symbols rather than commands. That is a one-line context test and it
reuses W2's index.

---

## 4. T -- the time-travel tracer

### 4.1 Why a recording, and how the reasoning differs from c2mp's

c2mp reached for a recording because it had no choice: MIR carries no source
positions, `mir-interp.c` has no hook, and the interpreter has no yield point,
so a *pause* would block the thread that owns the module and be
indistinguishable from a hang.

**None of that is true here.** Turmeric's interpreter pauses fine today --
`tur debug` and `tur dap` both do it, with frames, locals and in-frame
`evaluate` (`turi_debug_eval_expr`). So the case for a recording is not "a
pause is impossible", it is:

1. **Backwards.** The question a debugger cannot answer is "how did this value
   get to be 7", and stepping backwards is the answer. A pause cannot go back.
2. **The recording is nearly free to build**, because the pause handler already
   gets called at every node and already has an API for frames and locals.
   `turi_debug_set_pause_handler` + resume-step-in in a loop *is* a tracer.
3. **The web has no debugger at all.** `tur dap` is stdio; the wasm build has
   no way to expose a blocking pause loop to a browser tab. A trace is a byte
   buffer, and a byte buffer crosses the wasm boundary and gets scrubbed
   client-side with no protocol.

Also, and unlike c2mp: **no source instrumentation.** c2mp's S3 is a text pass
over C that inserts hook calls, with three rules about brace disambiguation and
two safety valves for when the rewrite does not compile. Turmeric skips that
entire section -- the interpreter is ours. Every bug in c2mp's S4 ("which call
is this?" -- sentinel addresses that do not order by depth, a host stack that
does not move, a sweep that has to scan from the bottom) is a symptom of not
having real frames. `turi_debug_frame_count` / `turi_debug_frame_at` give real
frames, so the tracer records the frame index it is given.

That is the whole of what c2mp spent its hardest section on, and it is already
built here.

### 4.2 The recorder

`src/turi/trace.c`, driven by the existing Phase 3 control API:

```c
/* Record every node the interpreter evaluates into a trace buffer.
 * Installs a pause handler that captures and resumes step-in, so the program
 * runs to completion without ever stopping for a user. */
TurTrace *turi_trace_begin(TuriEnv *env, const TurTraceOpts *opts);
void      turi_trace_end(TurTrace *t);
/* The recorded bytes. Valid until turi_trace_end. */
const uint8_t *turi_trace_bytes(const TurTrace *t, size_t *len_out);
```

The handler, once per evaluated node:

1. Read the frame stack (`turi_debug_frame_count` / `_frame_at`).
2. If the depth grew, emit `ENTER`; if it shrank, emit one `POP` per level.
3. Enumerate the innermost frame's locals (`turi_debug_frame_locals`) and emit
   a `STEP` carrying **only the bindings whose rendered value changed** since
   that frame's last step.
4. `turi_debug_resume_step_in(env)`.

**Deltas, not states** -- c2mp's S5, and the reason is the same: a step
carrying every live variable repeats the whole frame on every pass of a loop,
and in practice one or two values move per statement. c2mp measured a 93 MB
recording collapse to 425 KB once the frame accounting was right.

**No keyframes in the format.** The decoder builds its own snapshots every N
steps, which is the same work in a language that can afford it and keeps the C
side to one rule: write what changed.

### 4.3 The trace format

A length-prefixed binary stream, written to a `.turtrace` file or handed across
the wasm boundary as bytes:

```
header   magic "TURTRACE\0", u16 version, u32 site_count, site[site_count]
site     u32 file_id, u32 line, u32 col, u16 fn_name_id
record   u8 tag
  1 ENTER  u32 site, u16 depth, u16 frame
  2 STEP   u32 site, u16 depth, u16 frame, u16 n, change[n]
  3 POP    u16 depth, u16 frame
  4 OUTPUT u32 len, u8 text[len]        /* interleaved stdout, so the transcript
                                            can be replayed in step order */
change   u16 slot, u16 len, u8 repr[len]
```

Sites and names are interned in the header, so **nothing crosses as a string at
run time** (c2mp's S2). It crosses as bytes and not as a C string because it is
the one thing here that reaches megabytes and a NUL in a rendered value would
truncate it.

`OUTPUT` is a Turmeric-specific addition: c2mp's recorder had nowhere to put
program output, so scrubbing back through a `println` showed the console
unchanged. Interleaving it means the transcript rewinds with the cursor, which
is most of what makes a time-travel debugger feel like one.

### 4.4 The step cap, which is not optional

A recording of a runaway loop is a tab that dies. `TurTraceOpts.max_steps`
(default 200 000, matching c2mp's measured-useful value) ends the run through
the same unwind path a fuel exhaustion takes (`turi_env_set_fuel`,
`eval.h:221`, already exists and already has a sandbox default of 10M steps).
**Truncation is reported, never silent** -- the trace header carries a
`truncated` flag and every surface says so.

### 4.5 Surfaces

Three, in this order:

- **T1: `tur trace <file.tur> [-o out.turtrace]`.** Records and writes. With no
  `-o`, prints a summary (steps, frames, peak depth, bytes, truncated y/n).
  This is the smallest thing that proves the recorder, and it is testable from
  a fixture with `requires.interp-only` -- the trace is an interpreter
  behavior and `run.sh`'s compiled path does not share it.
- **T2: reverse execution over DAP.** Advertise
  `"supportsStepBack":true` and `"supportsReverseContinue":true` in the
  capabilities response (`src/turi/dap.c:656`) and implement `stepBack`,
  `reverseContinue` and `reverseNext` against a trace recorded at launch.
  **This is the highest-leverage item in track T**: VS Code and nvim-dap
  already draw the entire reverse-execution UI when a server advertises those
  capabilities, so Turmeric gets scrubbing, backwards breakpoints and a
  rewinding variables pane without writing a single widget. c2mp had to build
  all of it (`debug.js`, the panel, the gutter).

  The DAP session model becomes: launch records the whole run to completion,
  then *replays* it -- `stackTrace`, `scopes` and `variables` answer from the
  trace cursor rather than from a live env. `evaluate` is the one request that
  cannot be answered from a recording (there is no live frame to evaluate in);
  it returns an error naming that, rather than a stale value.
- **T3: the Try Turmeric timeline.** A scrubber under the console driven by the
  same bytes. Deferred behind T1 and T2 deliberately: if the format is wrong,
  T3 is the expensive place to find out.

### 4.6 Cost, and what is not traced

Expect roughly c2mp's order of magnitude -- it measured 1.6x to 13x depending
on how much of the program is statement dispatch -- but **measure it here
before writing a number into the docs**, because the constant is completely
different: c2mp paid an interpreted call per statement, and Turmeric pays a
handler call plus a locals enumeration per *node*, which is a finer grain and a
heavier body. If the locals enumeration dominates, the fallback is to capture
only on frame entry/exit and on `let`/`set!` nodes, which is a smaller trace
and a coarser scrub.

One trap worth stating up front, from CLAUDE.md: **the tree-walking interpreter
retains roughly 4 KiB per step of a trampolined loop**, so a 1e6-step fixture
already peaks at ~3.5 GiB RSS *without* a tracer. Trace fixtures must be small
programs, and the step cap is what keeps them small.

Not traced, and each for a reason:

- **Compiled programs.** The recorder is an interpreter feature. A native
  tracer is a different plan and would start from the `#line` emission behind
  `--debug` (`src/compiler/emit_core.c`).
- **Values that do not render.** A binding whose value has no `Show` gets its
  type tag and `?`, the same honest answer `turi_try_show_by_tag` already gives
  (`eval.h:81`).
- **Prelude and stdlib loading.** Recording starts at the arm point, as
  `turi_debug_arm_breakpoints` already establishes.

---

## 5. Phases

| Phase | Contents | Gate to the next |
|---|---|---|
| **S1** | `lsp_scope.c`, the binding table, `lsp_scope_lookup_at` | Fixtures: innermost-wins, binder-inclusive scope start, macro binding marked unrenameable |
| **A1** | Scope-aware `documentHighlight` | A local's uses stop at its function; minimap agrees |
| **A2** | `prepareRename` + `rename`, R1 (single document) | The shadowing fixture set in 2.2, including the `.tur.sweet` half |
| **A3** | R2 (workspace) + `textDocument/references` | Rename across two modules of one spice; refusal at a `:url` dep boundary |
| **W1** | Monaco single-line prompt, key reconciliation, `setBusy` focus fix | Existing `meta-commands.spec.js` still green; new history + Escape specs |
| **W2** | Session-scoped prompt document | An editor-only `defn` is *not* offered at the prompt before Run |
| **W3/W4** | Console hover; meta-command and `:doc <sym>` completion | Hover answers on an error line and stays silent on program stdout |
| **A6** | Cross-file verification pass; tab-switching go-to-definition | Cmd+click across tabs switches first |
| **T1** | `src/turi/trace.c`, `tur trace`, the format | A recorded `fib` fixture decodes to the expected step and frame counts |
| **T2** | DAP `stepBack` / `reverseContinue` / `reverseNext` | Stepping back into a returned frame shows that frame's values |
| **T3** | Try Turmeric timeline | Deferred; revisit after T2 |

S1 -> A1 -> A2 -> A3 is a hard chain. W1..W4 depend on nothing but each other.
T1 -> T2 -> T3 is a hard chain and is independent of everything above.

The natural split for two people is S/A/W on one side and T on the other. They
touch disjoint files; the only shared surface is `web/main.js`, and only if T3
happens.

## 6. Testing

- **Server:** extend `tests/lsp/session_test.c` -- it drives `lsp_session_handle`
  directly, which is the cheapest place to assert a `WorkspaceEdit` shape.
  Scope and rename cases go here, not in a fixture, because the assertion is
  about JSON rather than about program output.
- **Fixtures:** `tests/fixtures/` for `tur trace` output. Interpreter-only
  behavior gets `requires.interp-only` so `run.sh` hands it to `run-turi.sh`
  (CLAUDE.md's near-homograph warning applies: `requires.interp` is the
  opposite marker). Fixture sources stay ASCII.
- **Web:** `web/tests/` Playwright specs alongside `lsp.spec.js` and
  `meta-commands.spec.js`. Copy c2mp's two most valuable manual checks into
  specs: Escape dismisses the completion list *without* stopping a running
  program, and the arrow keys still walk history after an evaluation.
- **DAP:** a scripted client against `tur dap` asserting the capability
  response and a `stepBack` round trip. `tests/lsp/run-mcp-lsp.sh` is the
  closest existing harness shape to copy.
- Every suite invocation carries the 12-minute timeout (CLAUDE.md, strict rule).

## 7. Risks

- **The scope resolver disagrees with the elaborator.** The mitigation is that
  it is not a second traversal: it hangs off the same collection hook, so a
  binding form the elaborator understands and the resolver does not is a
  missing case rather than a divergence. Missing cases fall back to NULL, which
  every consumer reads as "global", which is today's behavior.
- **Rename corrupts a file.** The refusal table in 2.2 is the mitigation, and
  `prepareRename` is the reason it can be shown to the user *before* they
  commit. If a case is discovered that the resolver gets wrong, the fix is to
  add it to the refusal list first and to the resolver second.
- **Replacing the prompt `<input>` with Monaco regresses the prompt.** Real
  risk: the input is small, obvious, and works. Mitigation is that
  `meta-commands.spec.js` already covers the behavior that must not change, and
  the swap is one function (`initReplInput`) with a clean revert.
- **The tracer's per-node cost makes it useless.** Mitigated by measuring in T1
  before building T2, and by the coarser-grain fallback in 4.6. If the number
  is bad, T1 still stands on its own as a post-mortem tool.
- **The trace format churns after T3 exists.** Mitigated by ordering: the
  format is exercised by two consumers (T1's summary, T2's replay) before any
  UI depends on it, and the header carries a version.

## 8. Out of scope

- **Native (compiled) tracing.** 4.6.
- **A rename that crosses a fetched (`:url`) spice boundary.** It would need to
  edit files outside the workspace and rewrite `tur.lock`; prepareRename
  refuses instead.
- **Semantic tokens / inlay hints.** Adjacent and cheap once S1 exists, but not
  asked for and not needed by anything here.
- **Pausing the wasm interpreter from the browser.** c2mp's reasoning against
  it applies here for the same reason it applied there -- there is no yield
  point in a browser tab -- and the tracer is what makes it unnecessary.


---

## 9. Execution record

Written after the fact. The plan body above is left as it was proposed; this
section records what actually happened, including where the plan was wrong.

### 9.1 What landed

| | Where |
|---|---|
| **S1** scope resolver | `src/lsp/lsp_scope.{c,h}`, hooked from `src/main.c` and `src/web/wasm_lsp.c` |
| **A1** scope-aware highlight | `src/lsp/lsp.c` (`gate_admits`) |
| **A2** prepareRename + rename R1 | `src/lsp/lsp.c` |
| **A3** rename R2 + references | `src/lsp/lsp.c` (`workspace_scan`) |
| **A6** cross-file identity | `src/main.c` (`tur_collect_symbols` takes a logical path) |
| **W1-W4** the prompt | `web/main.js`, `web/lsp-client.js`, `web/try/index.html`, `web/styles.css` |
| **T1** the recorder | `src/turi/trace.{c,h}`, `tur trace` |
| **T2** reverse execution | `src/turi/dap.c` (`"replay": true`), `turi_trace_replay_*` |

Tests: `tests/lsp/session_test.c` (10 new cases), `tests/lsp/mcp_lsp_test.py`
(`test_lsp_inside_a_spice`), `tests/run-trace.sh` + `tests/fixtures/trace/`,
`tests/dap-replay-driver.py` driven from `tests/run-dap.sh`,
`web/tests/repl-intelligence.spec.js`.

### 9.2 Where the plan's repo facts were wrong

**Span offsets do not index the file on disk.** §1.2 specifies
`scope_start_off` / `scope_end_off` as byte offsets and assumes they can be
compared against the buffer the editor holds. They cannot, for two independent
reasons the plan did not anticipate:

- A **sweet-exp** buffer reaches the elaborator as *transformed* s-expression
  text, so every `Span` indexes a string the user has never seen. Diagnostics
  had always translated back (`render_snippet_ex`); nothing else did, because
  nothing else read offsets.
- A **`#lang` line** is stripped from the head of every file that has one, and
  `src` starts past it. Line numbering survives that strip (the directive's
  newline is left in place, so line 1 is simply empty), which is exactly why it
  went unnoticed -- every consumer so far read line/column.

`diag_translate_span` now undoes both, and `SourceFile` carries `head_offset`.
Without this, a rename in a `.tur.sweet` file edited the binder and missed
every use; the plan's own `.tur.sweet` fixture requirement (§2.2) is what
caught it.

**The reverse shadowing case is the destructive one, and §2.1 only names the
forward one.** The plan's `documentHighlight` sketch branches on "is this a
local?" and scans the whole buffer otherwise. That still paints a local's uses
when the caret is on the *global* it shadows -- and renaming from there
rewrites the local. `gate_admits` handles both: a local's marks are its scope,
and a global's marks are the buffer minus every region a local of the same name
covers.

**A cross-file rename needs each file's own locals, not just its imports.**
§2.2's R2 says "scan each file for occurrences, skipping any file that does not
`import` the defining module". That is necessary and not sufficient: a sibling
module can bind a local `total` *and* import the global `total`, and a textual
rewrite renames both. Each candidate file is now compiled for its own binding
table before it is edited. Rename is a deliberate, occasional action that can
afford a compile per importing file; `LSP_WS_ANALYZE_MAX` (200) bounds it, and
overrun refuses rather than emitting a partial `WorkspaceEdit`.

**A6 was not a verification pass.** §2.4 lists go-to-definition across modules
and completion from imports as things to *check* rather than build. Both were
broken, and badly: `tur_collect_symbols` passed no include dirs at all, and the
walk-up ran from the scratch file the LSP writes -- so a module inside a spice
came back with no symbol index whatsoever. `tur check` on the same file worked
the whole time, which is what made it invisible.

**The re-export gap does not exist.** §2.4's second "close" item -- a
re-exported name resolving to the re-export rather than to the defining `defn`
-- has no Turmeric analogue: `(export x)` for a name not defined in the module
is a hard error (`exported symbol 'x' is not defined in this module`), so the
situation cannot arise.

**The `setBusy` focus bug has no analogue either.** §3.1 says to copy c2mp's
S12.4 fix. Try Turmeric never disables the prompt during a run -- the only
`disabled` write is the one that *enables* it when the WASM boots -- so there
is nothing to blur and nothing to restore. What does transfer is the `Escape`
rule: the page has document-level Escape handlers for the docs pane and three
menus, so the prompt stops propagation.

**The step cap needed a decision the plan left open.** §4.4 says the cap "ends
the run through the same unwind path a fuel exhaustion takes". Setting
`turi_env_set_fuel(env, 1)` does that, and the alternative -- resuming
untraced -- was rejected because the recording would then describe a prefix of
a program whose answer came from somewhere the trace cannot show.

**The trace format grew two fields and lost one.** Ids are `u32` rather than
`u16` (a 65k ceiling on distinct names is the kind of limit that is fine until
a generated program walks into it), the header carries a name table (§4.3's
`site` references an `fn_name_id` with nothing to resolve it against), and
`ENTER` / `POP` carry no separate frame index -- `depth` already identifies the
frame, because frames are a stack.

### 9.3 Two bugs the first fixture was too small to reach

Both in T2, both found by giving the replay driver a larger fixture
(`tests/fixtures/dap-replay/`, ~8k steps) rather than reusing the live
debugger's 65-step one.

**A `continue` with no breakpoint ahead of it hung.** Breakpoint matching
seeked to each candidate step, and a seek rebuilds the whole state from the
start of the stream -- 160k rebuilds for one scan. Over 110 seconds and still
going, against 0.02s once each stop's site was indexed alongside its depth. A
65-step recording cannot tell the two implementations apart, which is the
entire reason the replay fixture is its own file and is deliberately not small.

**`setBreakpoints` during a replay session read freed memory.** A replay keeps
serving requests after the interpreter env is gone -- that is what replay *is*
-- and the handler still reached for it. `dap_end_session` nulls the pointer,
which is what makes the NULL guards that were already there true.

### 9.4 Measurements the plan asked for

§4.6 says to measure the tracer's cost rather than copy c2mp's number. On a
Debug + ASan build, a 20,000-iteration `while` loop: **0.11s untraced, 0.57s
traced (~5x)**, producing 80,006 steps in 1.2 MB -- about 15 bytes a step, with
deltas doing the work the plan predicted they would. That sits inside c2mp's
measured 1.6x-13x band. A Release measurement was not taken.

### 9.5 What was not done

- **T3, the Try Turmeric timeline.** Deferred, as §5 phases it: "if the format
  is wrong, T3 is the expensive place to find that out". The format now has two
  consumers (the `--dump` reader and the DAP replay) and a version byte.
- **The Playwright specs were written but not run.** The suite needs
  `web/turmeric.js`, and building it needs emscripten, which the execution
  environment does not have. `npm run build` bundles the changed JS cleanly
  (2,979 modules transformed), so the code parses and imports resolve; the
  behavioural assertions in `web/tests/repl-intelligence.spec.js` and the
  updated `meta-commands` / `smoke` / `diag` specs are unverified.
- **Semantic tokens and inlay hints.** Out of scope per §8, and still are.

### 9.6 One behaviour change worth calling out

`:help` in the web REPL is generated from the command table now, aligned on the
widest label. The old hand-written block spelled `:doc  <sym>` with two spaces;
the generated one spells `:doc <sym>`. `web/tests/meta-commands.spec.js`
asserted on the old spacing and was updated -- which is precisely the class of
drift the single table exists to prevent.
