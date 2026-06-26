## Turmeric Godot Binding -- In-Editor Debugger Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-25
> **Type:** Integration / Game Engine -- spun out of
> [godot-language-binding-plan.md](./godot-language-binding-plan.md)'s
> Phase G4 stretch goal.

---

## Generated docs convention

Phase progress/report write-ups generated as part of this plan live under
[`docs/artifacts/`](../../artifacts/), not `docs/upcoming/`.
`docs/upcoming/` is reserved for the plans themselves; artifacts are the
by-products of executing them.

## Why this is its own plan

The parent binding plan's G4 ("Editor Niceties") parked the debugger
as a stretch: "if it slips, ship without it and document the gap."
It slipped. The G4 MVP shipped with the syntax highlighter and
completion only; debugger plumbing was never started. That decision
is right -- breakpoints + stack-frame inspection across a
GDExtension-hosted scripting language is its own multi-week stream
with its own failure modes, not a small ergonomic polish step.

This plan covers what landing it actually entails so future planning
has a real shape to consume instead of "ship the debugger."

---

## What "the debugger" means in Godot 4

Godot exposes a `ScriptLanguageExtension::debug_*` family that the
in-editor remote debugger UI calls into:

| Hook | What Godot wants back |
|---|---|
| `debug_get_error()` | Last error string (one-line). |
| `debug_get_stack_level_count()` | Number of frames currently paused on. |
| `debug_get_stack_level_line(int)` | Source line for a given frame. |
| `debug_get_stack_level_function(int)` | Function name for a frame. |
| `debug_get_stack_level_locals(int, int max_subitems, int max_depth)` | `{name: value}` map of locals at that frame. |
| `debug_get_stack_level_members(int, ...)` | Instance-level members (Godot `Object` properties). |
| `debug_get_stack_level_globals(int, ...)` | Globals. |
| `debug_parse_stack_level_expression(int, String expr, int max_*)` | Evaluate an expression in a frame's environment. Used by the watch panel. |
| `debug_get_current_stack_info()` | A Dictionary list of {file, line, function}. |
| `debug_get_globals(int max_subitems, int max_depth)` | Project-wide globals (we don't really have any -- return empty). |
| Breakpoint set via `Script::set_source_code` -> `ScriptDebugger`. |

Godot's debugger model is **synchronous**: when a breakpoint hits, the
script-language is expected to block until the user resumes, while
servicing the above queries from the debugger UI in the meantime.
This shape is incompatible with the interpreter's current
"top-down evaluate-and-return" model -- there is no suspendable frame
stack to interrogate after-the-fact.

---

## Required substrate work in libturi

The debugger queries are not implementable against today's
tree-walking evaluator. Each row below is a substrate prerequisite,
ordered by how blocking it is.

### B1. Frame stack you can read

The evaluator walks the AST recursively in C; "frames" exist only
as C stack frames. We need a parallel data structure -- an array of
frame records (function name, source span, captured environment
pointer, line of the current sub-expression) maintained by
`eval.c` as it descends and ascends.

- Push on closure-call entry.
- Pop on closure-call exit.
- Update the "current sub-expression" line per evaluated form.

This frame stack lives on the `TuriEnv` (one per script) so the
debugger thread can read it without racing the eval thread.

### B2. Suspendable evaluation

The evaluator currently runs to completion inside one Godot call
(e.g. `_process`). For breakpoints to pause execution, eval needs to
be able to *yield* at a chosen point and resume from the same place
later.

Two viable implementations:

- **Fiber-based (preferred)** -- run script eval on a fiber the
  debugger thread can park/unpark. libturi already has a fiber
  scheduler (used for async); same primitive.
- **Continuation-style** -- thread eval state through a sum-typed
  return so each step is interruptible. Larger refactor, but no
  fiber dependency.

The fiber path piggybacks on existing async work; it is the
recommended choice.

### B3. Source-span tracking that survives macro expansion

The current AST loses source spans through quasiquote / `defmacro`
expansion. A breakpoint set on the user's source line has to map to
the *expanded* AST node it corresponds to. Span propagation through
the macro expander is its own correctness pass.

This work is partly shared with the existing
`docs/artifacts/debugger-spans-audit.md` work in `tur` proper. Land
that first; the Godot binding consumes the result.

### B4. Breakpoint table

Plain set of `(source_path, line)` pairs the evaluator checks before
each form it evaluates. A hash-table lookup keyed by interned source
path + line. The cost has to be low enough that an eval loop with
breakpoints disabled is indistinguishable from one without -- this
implies a feature flag the evaluator branches on early.

### B5. Locals-at-frame inspection

Given a frame index, walk the captured environment chain at that
frame and project each binding as a Godot `Variant` via the existing
`variant_marshal.cpp`. Closures and structs come out as opaque
handles with a `Show`-style summary; scalars round-trip.

`debug_parse_stack_level_expression` is essentially "run this string
as Turmeric in the given frame's env" -- requires the frame's env to
still be alive (B1) and the marshaller (existing).

---

## Phases

### Phase D1 -- Substrate (~3-4 weeks)

- B1: frame stack on `TuriEnv`. Read-only access from outside the
  eval thread.
- B3: source-span propagation through macro expansion (or accept
  best-effort spans and document the gap).
- B4: breakpoint table + per-form pre-eval check with a flag-gated
  cold path.

This is the load-bearing risk -- everything else assumes a working
frame stack and breakpoint check.

### Phase D2 -- Suspend / resume (~2-3 weeks)

- B2: fiber-based script eval. Each script-method dispatch entry
  point (cb_call from Godot) spawns or unpark-jumps into the script
  fiber; a breakpoint trap parks the fiber and signals the
  debugger thread.
- Resume / step-into / step-over / step-out as fiber unpark
  requests.

### Phase D3 -- Godot wiring (~1-2 weeks)

- Implement the `debug_*` hooks against the substrate.
- `debug_get_stack_level_locals` walks frame env via B5.
- `debug_parse_stack_level_expression` runs an ad-hoc eval in the
  frame's env.
- Hand-test in the Godot editor: set a breakpoint on a line in
  `paddle.tur`, run, hit it, inspect `self`, step over, resume.

### Phase D4 -- Demo + docs (~1 week)

- A short screencast in the README showing breakpoint + locals.
- `docs/guides/godot-binding-guide.md` debugger section with the
  current capability matrix and known gaps (e.g. macro-expanded
  spans may point to surprising lines).

---

## Risks

- **Fiber-based eval is invasive.** The eval entry point currently
  assumes a synchronous return up the C stack; making it
  fiber-yieldable touches every form's eval path. Mitigation: do
  D2 behind a `--debugger` feature flag so non-debugger runs keep
  the current code path bit-for-bit.
- **Source-span loss through macros** is a real correctness issue
  -- a breakpoint on `(defmethod _ready ...)` may land inside the
  defgodot-script macro's expansion of `(defn _ready ...)`. The
  span audit (already a `tur` work item) is the upstream fix.
- **Threading.** Godot calls `cb_call` on the main thread; if the
  debugger thread is also reading frame state, lock-free reads or a
  per-script mutex is required. Mutex on the rare debug query is
  fine.
- **Hot-reload interaction.** A debugger breakpoint surviving a
  script reload is out of scope for v1; document and move on.

---

## Out of Scope

- **Heap walker / object inspector.** v1 shows struct fields as
  string Repr; an expandable tree-view comes later.
- **Conditional breakpoints.** Plain line breakpoints only.
- **Profile / CPU sampling.** Different thread; not in this plan.
- **GDScript-style "remote debugger over TCP" for headless runs.**
  Editor-only debugging is the v1 target.

---

## Estimate

~6-9 weeks of focused work, with D1 + D2 as the load-bearing risk.
Sequencing matters: D1 must land before D2 is meaningful, and D3 is
mechanical once D1 + D2 are stable. This is the kind of milestone
that pays off in editor UX but does *not* unblock v1 ship -- the
parent plan explicitly says "ship without it and document the gap"
is a valid landing.

---

## Success Criteria

- A user can open `paddle-pong-tur` in the Godot editor, set a
  breakpoint on a line in `ball.tur`, hit Play, see execution pause
  at that line, inspect `self`, evaluate `(+ vx vy)` in the watch
  panel, step over the next form, and resume.
- Breakpoints survive across `_process` re-entries (i.e. the
  breakpoint table is per-script, not per-method-call).
- Headless mode is unaffected -- the substrate's `--debugger` flag
  defaults off and the eval path is byte-identical to today's when
  the flag is off.
