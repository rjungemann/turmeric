# Run-loads-definitions-into-REPL (DrRacket-style) -- plan

## Goal

After pressing Run on a `.tur` file in the Lite XL editor, every
top-level `defn` / `defmacro` / `defstruct` / `def` from the file is
bound in the REPL pane, so the user can call `(hello)` at the prompt
and reach a function they just edited above. Same model as DrRacket's
Definitions/Interactions split.

Today: `cmd+r` shells out to a one-shot `tur --interpret <file>` that
runs the program and exits. Nothing about the file's definitions
survives the run. The ReplView is a separate process with its own
empty env.

## North star

```
1. User types/edits file in DocView.
2. cmd+r:
     - save the buffer
     - load the file into the ReplView's running interpreter
     - print stdout/stderr (and main's return value, if any) in the REPL
     - leave every top-level binding in scope at the prompt
3. User types `(hello)` at the `tur>` prompt and it works.
4. Re-pressing cmd+r reloads cleanly: re-defines bindings, drops
    stale-but-not-redefined ones in lockstep with DrRacket's behavior.
```

## What's already in place

- `tur repl` (src/turi/repl.c) implements meta-commands `:help`,
  `:quit`, `:type`, `:doc`, `:reload <file>`. The interpreter already
  has the load-a-file-into-current-env primitive we need.
- ReplView (tools/lite-xl/turmeric.lua) owns a long-lived `tur repl`
  subprocess and can push arbitrary text to its stdin via
  `send_buffer_text` / `send_input`.
- `cmd+shift+l` (`turmeric:repl-reload-buffer`) already pipes raw
  buffer text into the REPL. That gets us most of the way -- it's the
  wrong UX (no save + run combined, no clear delimiter, no "load main"
  semantics), but the wire is there.

## What's missing

1. **`cmd+r` doesn't talk to the REPL.** Today it spawns a fresh
   `tur --interpret` subprocess. That has to change: `cmd+r` should
   target the ReplView's running interpreter instead.
2. **`:reload` semantics in `tur repl` need to be verified.** Per the
   header comment it "reloads a file", but does it:
   - Re-elaborate the file in the current env (so re-`cmd+r` after an
     edit redefines the same bindings)?
   - Drop bindings that disappeared from the file between runs?
   - Surface compile-time diagnostics inline?
   Each of these is a separate behavior to check. Phase L1 is
   load-bearing reconnaissance.
3. **`(main)` call convention.** DrRacket runs the program; what's the
   equivalent here? Two reasonable options: always evaluate top-level
   forms top-to-bottom (so a bare `(println "hi")` runs), and
   additionally call `(main)` if defined. The interpreter already does
   the first; the second is a one-line meta-command to add.
4. **REPL-side framing of run output.** The REPL pane needs to clearly
   delimit a Run cycle from prior interactions: a header line, the
   output, a "ready" line, then return to the prompt.

## Phased plan

### L1 -- Recon on `tur repl :reload` semantics (0.5 day)

Read `src/turi/repl.c` (the `:reload` handler) and `src/turi/eval.c` to
answer:

- Does `:reload` re-elaborate in the same `TuriEnv`, or does it spin
  up a fresh sub-env?
- If the same env: are removed bindings dropped, or do they leak?
- How are parse/type errors surfaced -- do they hit stderr, return a
  Result, or panic the REPL?
- Is there a one-shot "evaluate this string in the current env"
  meta-command (e.g. `:eval`)? If not, what's the closest primitive?

Output: a 1-page memo at `docs/notes/tur-repl-reload-semantics.md` so
the rest of the plan can be honest about what's free and what needs
new compiler-side work.

### L2 -- Wire `cmd+r` into the ReplView (1 day)

Replace `turmeric:run-file`'s current `tur --interpret <file>` spawn
with a path that targets the active ReplView:

- Save the buffer.
- `open_repl_view("down")` if the pane isn't already open.
- Push a visible header into the pane: `;; run: <abs-path>` (with a
  timestamp so the user can see what's fresh).
- Issue `:reload <abs-path>` (or whatever L1 turns up as the right
  meta-command) to the REPL stdin.
- If the file defines `main`, follow with `(main)` (probe via
  `lsp-lite` `doc main` -- already wired -- or just always send
  `(main)` and let unbound-name errors fall through to the REPL).

Acceptance:

- Edit `examples/cellular-automata.tur`. Press `cmd+r`. See the
  program output in the REPL pane.
- At the `tur>` prompt, call any defn from the file -- it resolves.
- Edit the file, press `cmd+r` again. The new definition is visible
  at the prompt; calling the old name still works if it's still in
  the file, fails cleanly if it isn't.

### L3 -- Diagnostic surfacing (0.5 day)

`:reload` parse / type errors today land in stderr alongside REPL
prompt noise; this is acceptable but rough. Tighten:

- Tag the ReplView line kind for stderr from a `:reload` as `err`
  (already styled distinctly).
- For each `path:line:col:` line the compiler emits, expose a
  ReplView click-handler that jumps to the source location in the
  DocView. Reuse the same regex the plugin already runs against
  diagnostic lines from `tur check`.

Acceptance: a syntax error in the file shows up as red text in the
REPL with a clickable file:line:col that opens the editor at the
right spot.

### L4 -- "Definitions changed since last run" indicator (0.5 day, nice-to-have)

DrRacket dims the Interactions pane when the Definitions pane has
unsaved changes and shows "Definitions modified -- press Run to
reload." Mirror this lightly:

- Hook into the DocView change event for `.tur` buffers.
- If the buffer is modified since the last `:reload`, draw a small
  `(stale)` chip in the ReplView header.
- The chip disappears as soon as the user presses `cmd+r`.

Acceptance: edit the file after a Run; the ReplView header shows
`(stale)`; press Run; the chip clears.

### L5 -- Optional: `cmd+enter` to send selection / current form to REPL (1 day, deferred)

A common DrRacket-adjacent affordance from Emacs / SLIME / Calva:
position cursor inside a top-level form, press a key, that form alone
is evaluated in the REPL. Less invasive than reloading the whole file.

- Find the enclosing top-level form via paren-balance walk on the
  buffer text (cheap; same trick the indentation rule uses).
- Send `(let [] <form>)` -- or just the form raw if `:reload` doesn't
  scope what it loads -- to the REPL stdin.
- Echo the form into the pane prefixed with `eval< ` so it's
  recognizably ad-hoc and not part of a "Run" cycle.

Acceptance: cursor on a `(println "hi")` form, press `cmd+enter`, see
`hi` print in the REPL without disturbing the rest of the env.

## Non-goals

- A full debugger / breakpoint UI in the REPL. Covered separately by
  the debugger plan; this plan does not block on it.
- Compiled-mode REPL. The path here is interpreter-mode (`tur repl`,
  which is tree-walking). A future "AOT REPL" would slot in by
  changing `config.plugins.turmeric.repl_subcommand` -- one config
  knob, no other changes.
- Sandboxed evaluation / "fresh env per Run." If the user wants a
  clean slate, they can `turmeric:stop-repl` then `turmeric:start-repl`.
  Adding a `cmd+shift+enter` "Run Fresh" affordance is on the table
  but not on the critical path.

## Risks / open questions

1. **`:reload` may not re-define cleanly.** If the interpreter
   genuinely needs a fresh env for clean re-loads, L2 has to coexist
   with a "restart subprocess on every Run." That's still acceptable
   -- the REPL would just lose its history per Run -- but it
   undercuts the "type at the prompt, edit above, re-run, keep going"
   loop. L1 has to confirm before L2 commits.
2. **Compile-time errors and partial loads.** If a file is broken,
   `:reload` may load nothing, half, or panic. Behavior here drives
   how L3 surfaces things; harden once L1's memo lands.
3. **Big-file performance.** `:reload` on a 10kloc file blocks the
   REPL until elaboration is done. Tree-walker elaboration is fast
   but not free; if this turns into a real wait, add a "loading..."
   placeholder in the pane.

## Effort summary

| Step | Effort | Blocker |
| --- | --- | --- |
| L1 -- :reload recon memo  | ~0.5 day | none |
| L2 -- cmd+r -> REPL :reload | ~1 day | L1 |
| L3 -- diagnostic surfacing | ~0.5 day | L2 |
| L4 -- stale-buffer chip | ~0.5 day | L2 |
| L5 -- send-form-at-cursor | ~1 day (deferred) | L2 |

**Critical path to "press Run, then call (hello) at the prompt": ~1.5 working days (L1 + L2).**
