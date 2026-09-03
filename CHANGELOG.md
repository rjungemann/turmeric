# Changelog

All notable changes to Turmeric are documented here.

## [Unreleased]

### Changed

- **Announced ahead of the flip: `(some p)` over a `:non-null` payload will
  no longer be able to carry NULL once the Option niche graduates.** The
  niche is still `--enable=option-niche` today, and nothing changes until it
  defaults on; this entry exists so the break is in the release notes before
  the release that makes it, not discovered in it. When it lands, an
  `(Option P)` for a `:non-null` opaque or a compiler-lowered heap collection
  is carried AS its payload pointer -- 16 bytes to 8, `(none)` as NULL, no tag
  word. The representation spends the bit pattern `0` on `None`, so a `Some`
  whose payload is null has nowhere left to live.

  **Breaking for inline-C that builds an Option over such a payload.**
  `tur_some_ptr(0)` produces a legal value today that `some?` answers true
  on; under the niche it is a diagnostic naming the type and the violated
  declaration (`a carrier Some with a NULL payload crossed into a
  niche-represented Option -- the payload type's :non-null declaration was
  violated`), raised at construction or at the carrier crossing, whichever
  comes first. It is a diagnostic with a message, not a crash. The fix is one
  line and is almost always what the code meant: return `tur_none()` for the
  absent case. If a payload type genuinely has a valid null, drop `:non-null`
  from its `defopaque` -- that un-elects it from the niche and restores the
  16-byte tagged form, at no other cost.

  A *provable* violation (the literal `0` ascribed into a `:non-null` handle)
  has been `TUR-E0303` at elaboration since 0.41.0 and is unaffected. The
  flip itself is tracked in `docs/upcoming/sr3-option-niche-plan.md`; when it
  happens, this entry moves under that release's `### Changed` with the
  "Announced ahead of the flip" lead removed.

### Fixed

- **A fresh `Option`/`Result` over a value struct passed to a class method
  no longer leaks its payload box.** `(enc (some (make-struct Box ..)))`
  kept the `Box` copy for the process lifetime although the instance only
  read its argument: the drop-after-consumer stamp lived in the ordinary
  call path and a class-method call never ran it. The resolved instance's
  inferred non-retention mask now decides there (per monomorph at emit when
  the receiver is abstract). A let binding through `ok-val` / `err-val` /
  `unwrap` of a fresh producer is freed at scope exit as the payload's only
  holder, and a return-dispatched producer's carrier cell is drained against
  the instance that ran, so the struct copy inside it is freed too. Closes
  `docs/archive/value-struct-payload-sum-monomorph-box-has-no-owner.md`.

- **A callee that stores its `Option`/`Result` argument is no longer inferred
  non-retaining.** `(defn stash [o : (Option Box)] : int (vec-push! store o) 1)`
  got the non-retention bit because the confinement walk modelled a callee
  only through its result, so the caller freed its fresh `(some Box)` after
  the call and the container read it back freed (a use-after-free since
  2026-09-02, silent without ASan). Under the sum walk a hand-off to a callee
  that is neither an audited reader nor proven non-retaining is an escape.
  Pinned by `sum-payload-stashing-callee-not-dropped`.

- **A class method whose result is the class variable can mint an `Option`
  over a value struct in a constrained instance.** Three `cc` errors on a
  shape `tur check` accepted: the ctor-argument seam deref'd a re-dispatched
  by-value result as the carrier; a dictionary slot's base clone was declared
  with a by-value result while its tail spilled to the carrier; and
  `[(Option Box)]` as an instance head read `Box` as a type variable and made
  the method's own parameter move-only. All three fixed;
  `docs/archive/return-dispatched-sum-mint-in-constrained-instance-miscompiles.md`.

- **An inline-C body that boxes a value-struct payload into a declared
  `(Option T)` / `(Result T E)` hands that payload over with the box.** Such a
  producer is fresh by declaration now, so the payload is freed with the cell
  instead of leaking; the guide states the contract (the payload must be a
  fresh allocation, like the box).

- **`--enable=option-niche`: a niche `(Option P)` Vec element is stored as its
  payload word (CE1/CE2 of the container-element-form plan).** A
  `(Vec (Option String))` used to pay a heap carrier box per element under the
  niche exactly as the by-value default does; the element now lands in its
  slot as the String pointer (None is 0), and every read hands it back. 2e6
  elements: 17.8 MB / 0.018 s against 79.7 MB / 0.08 s. `TUR-E0714` refuses a
  niche element stored through a fully erased receiver, the one shape that
  cannot decide the slot convention. Also fixed on the way: a generic
  `(defn push-it [A] [v : (Vec A) x : A] (vec-push! v x))` specialized for an
  Option element double-wrapped the value -- a silent blank read under the
  niche and a `cc` error on the default path.
  `docs/upcoming/container-element-form-plan.md`.

## [0.42.2] -- 2026-09-01

### Added

- **`tur dap` serves a recording as a timeline**, not just as a sequence of
  steps. DAP describes execution as one step after another and has no
  vocabulary for an axis -- correct for a live debuggee, where there is nowhere
  to scrub to, and wrong for a recording, which is an axis. Three custom
  requests add one, advertised as `supportsTurmericReplayTimeline`:
  `replayInfo` (how many steps, where the cursor is), `replaySeek` (jump to
  step N, clamped, reporting where it landed) and `replaySites` (where steps
  are and how deep, by explicit index or downsampled to a bucket count). All
  three refuse in a live session naming the reason, because a client that asks
  has a scrubber in mind.

  `replaySites` returns position and depth **together**, which is the shape Try
  Turmeric's `trace-site-at` already uses: a timeline's cursor readout wants
  `file:line` and a depth ribbon wants `depth`, over the same steps. A bucket
  reports its range's *maximum* depth and the site of the step where that
  maximum occurred, so a deep call between two samples is not erased and
  clicking a ribbon spike lands where the spike is.

  None of this is new capability in the reader -- `turi_trace_replay_seek`,
  `_steps`, `_depth_at` and `_site_at` already existed, and the last two are
  index reads. What was missing was any way to reach them over the wire, and
  the alternatives are worse than they look: a slider whose range is a guess,
  and a seek approximated by repeated `stepBack`, which rebuilds state from the
  start of the stream once per candidate and turns a scan of an 80k recording
  from milliseconds into a hang.

- **The replay console rewinds.** Forward motion still appends through ordinary
  `output` events. Backward motion could not: the transcript at the new cursor
  is a prefix of what the client was already sent, and a delta cannot express a
  truncation -- so the old code sent nothing and left the console showing
  output from steps the cursor had rewound past. A backwards seek now emits a
  `replayOutput` event carrying the whole transcript to be used in its place.
  Whole-transcript rather than a cut offset, because a client that missed an
  earlier event would otherwise cut in the wrong place and never know. Clients
  that do not recognise the event are exactly as they were.

### Changed

- **The website is rebuilt around one canonical site map.** The topbar,
  sidebar, mobile drawer and footer are generated from a single pair of lists
  shared by `web/site.js` and the three page generators (`tools/gendocs.py`,
  `tools/genguides.py`, `tools/genspices.py`), so hand-written pages and
  generated ones -- guides, API docs, spices -- can no longer disagree about
  what is on the site. Every chrome link carries a `title` describing where it
  goes, the mobile drawer shows the same site map the desktop rail does, and
  the home page's install step became a tabbed set of install methods. The
  tour was reworked to match.

### Fixed

- **A recording's last step now shows what the program printed.** A replay
  transcript holds the output produced strictly before the cursor's step, and a
  program whose final act is a `println` drains it after the final STEP -- so
  the last index reported an empty transcript. Measured: `outputLength: 0` at
  step 24020 of 24021 for a fixture that prints "done" and exits. An empty
  console at the end of a run that printed reads as a broken timeline rather
  than a precise one. The final step now concatenates every OUTPUT record, the
  same special case and for the same reason as Try Turmeric's
  `turi_wasm_trace_output_full`.

## [0.42.1] -- 2026-08-31

### Changed

- **`tur trace` records one step per expression, not per source line.** The
  recorder drove the debugger with step-in, whose stop predicate is
  line-granular, so a recording's resolution was a source line -- the wrong unit
  in a Lisp, and more so in Turmeric, where neoteric `f(g(x))` and sweet-exp `$`
  chains exist to put more on a line rather than less. A loop whose body fit on
  one line collapsed into a single step, with the induction variable jumping
  from its first value to its last in one delta and every iteration's output
  arriving in one drain; and fidelity depended on formatting, the same loop
  recording 3 steps on one line and 23 across four. Both spellings now record
  58. `tur debug` stepping is unchanged -- line granularity is what a human
  drives by hand and what DAP speaks -- and `--lines` selects the old
  granularity.

- **The `.turtrace` format is v2.** A site carries a column range rather than a
  bare column, and the header records which granularity a recording was taken
  at. The column was always in the format but named nothing under line stepping:
  it was whichever node landed first on a newly entered line. v1 recordings
  still read back. The step cap moved with the unit (200,000 -> 1,000,000
  native, 50,000 -> 250,000 in the browser) -- a cap bounds the recording, but
  what it means is how much of a program fits under it.

- **`tur run` matches attributes by name and refuses unknown ones**, and aborts
  on a builtin failure rather than continuing with an empty string.

### Added

- **`tur run` gains Justfile parity on parameters, modules and builtins**: named
  and flag parameters via `[arg(...)]`, `mod` and imports, backtick evaluation,
  and `os_family` / `path_exists` / `replace` / `join` / `error`.

- **The Try Turmeric timeline highlights the expression** inside the current
  line, which is the visible half of recording per expression; the toolbar
  scrolls when it overflows.

### Fixed

- **Two silent-ignore holes in `tur run`** where a parameterized attribute was
  accepted and then quietly dropped.

- **`tur run` runs recipes from the Justfile's directory** and honors `[no-cd]`.

- **A node was hooked twice by the interpreter's debugger** when the driver
  handed a black-box node to `eval_expr`. Line-granular stepping hid it -- the
  duplicate shares a line -- so it surfaced as doubled records the moment the
  recorder began asking for every node.

## [0.42.0] -- 2026-08-30

### Added

- **`tur trace` -- a time-travel recorder, and reverse execution in `tur dap`.**
  `src/turi/trace.c` records every node the tree-walker evaluates as deltas
  rather than states: a step carries only the bindings whose rendered value
  moved, so 80006 steps cost 1.2 MB -- 15 bytes a step. `tur dap` `launch` with
  `"replay": true` then serves the whole session from that recording, so
  `stackTrace`, `scopes` and `variables` answer from a trace cursor. That is
  what makes `stepBack`, `reverseContinue` and `reverseNext` answerable at all
  -- a pause cannot go back. VS Code and nvim-dap draw the reverse-execution UI
  off `supportsStepBack`, so there is no editor-side widget here.

- **A time-travel timeline in Try Turmeric.** Trace is a second button beside
  Run (and a `:trace` command at the prompt): it records the tab's program under
  the interpreter and turns the console area into a scrubber -- a slider over
  the run, step forward and backward, the editor gutter following the cursor,
  each live frame's bindings at that point, and the transcript rewinding with
  it. The recorder was already compiled into the wasm module and simply
  unexported; the module is byte-identical in size either way, so this costs no
  download.

- **The Try Turmeric prompt gets completion, hover and its own LSP document.**
  The `turi>` prompt is a single-line Monaco editor backed by
  `file:///project/repl.tur` rather than an `<input>` -- not for the look, but
  so that completion, hover and signature help at the prompt are the providers
  that already exist instead of a second widget stack built over a text field.

- **Lexical scope in the language server -- scope-aware highlight, rename and
  references.** The symbol index knew only global bindings, so every consumer
  answered a textual question when it had been asked a lexical one: a parameter
  named `x` highlighted every `x` in the file, and rename could not be written
  at all. Each local binding now carries the region it is visible in, as two
  ranges whose gap is the binding's own initializer -- so `(let [x (+ x 1)] x)`
  resolves the init's `x` to the OUTER binding while a caret on the binder still
  resolves to the inner one.

- **Serializing a continuation inside an open `bt-scope` is refused.** Both the
  host codec and the emitted `tur_serial_cont_serialize` now report the trail
  depth and the count of outstanding trailed writes instead of producing a blob.
  A serialized continuation carries control; the undo information that would put
  the scope's writes back is process-local and does not travel with it, so such a
  blob would deserialize into a world where those writes either never happened or
  can never be unwound.

### Changed

- **`backtrackable-state` graduated -- `stdlib/trail.tur` is always available.**
  The trail (mark/undo cells with per-cell, per-write and per-level opt-out) no
  longer needs `--enable=backtrackable-state`; the experiment row is deleted and
  the module is an ordinary autoload. What held it at `prototype` was one open
  question -- plan 3.5's multi-shot re-entry -- now decided: **the checked error
  is permanent and snapshotting the live trail segment on capture is declined.**
  The SX0 curve settled the cost half (a snapshot is at best at parity with
  replaying the writes at 5.0 ns each, and is paid at *every* capture rather than
  only on the branch taken), and the semantics half went the same way (a
  symmetric snapshot restores the learned clause away, which is the one thing a
  backjump must keep). Writing the fixtures turned up a stronger guard than
  either: `Mark`, `BtCell` and `GCell` have no `Clone` instance, so a multi-shot
  `cloneable-shift` **cannot capture a trail handle at all** -- `TUR-E0014` at
  compile time, now pinned so it cannot regress.

  Because trail.tur now prepends to every compile, 148 codegen snapshots moved.

- **The trail works under `--interpret`.** `stdlib/trail.tur` is entirely
  inline-C, which the tree-walker cannot execute, so making it an unconditional
  autoload would have left a whole module resolving to nothing outside the
  compiled path. It is now shimmed for the interpreter -- and shimmed by
  *calling the same `src/runtime/trail.c`* rather than reimplementing it, so
  there is one trail, not two that can drift. `tur --interpret`, `tur eval`,
  `tur repl` and the web REPL all have the full surface; five fixtures now run
  under both harnesses and produce byte-identical output, including the
  `bt-depth` counts that pin "a thousand writes cost one trail entry".

  Serializing a continuation is the one compiled-only behavior, and it is not a
  gap: under `--interpret` `tur_serial_cont_serialize` is an in-process deep
  copy rather than a byte codec, so no blob outlives the trail and there is
  nothing to refuse.

- **`::` between an integer kind and a float kind is refused, and both meanings
  are named.** It meant two different things depending on where the value came
  from -- `(:: (.n m) :float)` means the NUMBER, `(:: (list-head c) :float)`
  means the BIT PATTERN -- and both operands are statically `:int`, so the
  same-size rule answered "bits" for both, silently. A small integer read as
  IEEE bits is a denormal, so `mixedfold` returned 0.25 instead of 3.25: a
  dropped term, not a garbage one. Say which you mean -- `int->float` /
  `float->int` (`stdlib/math.tur`) to convert the number, the new `float->bits`
  / `bits->float` (`stdlib/bits.tur`) to reinterpret. Integer literals stay
  exempt and keep converting. The interpreter registers the new pair as natives,
  which closes a compiled-vs-interpreted divergence rather than adding one: the
  tagged model could never implement a bit-reinterpreting `::`, but once the
  author has said which reading they meant there is nothing left to guess. All
  four spellings now agree on both paths.

- **`stdlib/arrow`: real `ArrowLoop` feedback via `LoopCell`.** The fed-back `d`
  of `ArrowLoop` at `(->)` was a sentinel `0`, so the instance was only correct
  when the looped arrow never read it. Turmeric is strict, so `d` cannot simply
  BE the value the same run is about to produce -- but indirecting through a
  two-word heap cell `{ filled, value }` splits "the value is written" from "the
  value is read", which is the only thing laziness was buying here.
  `arrow-loop` / `arrow-loop-lazy` fill it with the `d` output when the run
  returns (knot-tying, so a deferred read observes the `d` this run produced);
  `arrow-loop-fix` seeds it and refills per pass until `d` stops moving or fuel
  runs out. One shared protocol, so a single looped arrow works under any of
  them.

### Fixed

- **`any` payload boxes leaked once per widen.** A value RETURNED as `any`, or
  handed back by a callee that boxed it, leaked 16 bytes a turn under
  LeakSanitizer in three residual shapes -- the earlier argument-position fix
  could not help there, because a caller-frame copy would dangle. Ownership is
  now settled where the value lands: a non-escaping local's box dies with its
  scope, an owned temporary is dropped after the call that consumes it, the drop
  fires at early exits as well as the normal one, and a non-retained widen stays
  in the caller's frame. `any` also joins the CPS subset, so a `perform` beside
  one lowers.

- **A local callee was spelled as two different C identifiers.** Calling an
  ascribed `:fn` param -- `((:: f (fn [int] int)) v)` -- hoists the callable head
  into a synthetic binding whose declaration and use went through different
  naming rules, so cc rejected the undeclared one: a clean build break on a
  documented spelling. Both ends now name it by the same rule.

- **Wide by-value aggregates crossing the poly-to-fat boundary.** The three
  poly-to-fat ABIs disagreed about an argument too wide for the carrier; they now
  bridge it through the fat-box carrier and agree.

- **`bt-level` and `bt-depth` read an unspecified upper half.** Both C functions
  return `uint32_t` while `stdlib/trail.tur` declared them `:int` (`int64_t`), so
  the result's high 32 bits were whatever the callee left in the register -- a
  level could in principle read as 4294967297. Found while writing the serialize
  guard. Both now go through `tur_trail_level_i64` / `tur_trail_depth_i64`,
  matching how a mark is already packed across that boundary.

- **Editor-side resolution fixes.** DAP breakpoints match against the recorded
  site rather than a rebuilt state; the language server resolves a buffer's
  spice from where the file lives rather than where it was written; a local
  whose scope start cannot be computed is dropped instead of indexed. Try
  Turmeric starts a recording from a fresh session, aligns the dialect button
  with the rest of the toolbar, and does not register the service worker on
  localhost.

## [0.41.0] -- 2026-08-28

### Added

- **Three structural diagnostics for shapes that previously compiled into
  nothing.** `TUR-E0711` rejects a non-definition form at `defmodule` top level
  (it was silently never evaluated); `TUR-E0713` rejects a definition in tail
  position of a function body (there is nothing to return); `TUR-E0712` bounds
  the emitter's expression walk so a pathologically nested expression reports
  instead of running the C stack out. See `docs/guides/syntax-guide.md`.

- **`:non-null` is declarable on a `defopaque`,** replacing the option-niche
  allowlist's hand-maintained opaque rows. Ascribing the literal `0` into such a
  type is now `TUR-E0303` at elaboration rather than an abort at the niche `Some`
  constructor at runtime; a *computed* zero is not provable there and still falls
  through to the runtime check. `tur explain TUR-E0303` carries the rationale.

### Changed

- **A `defopaque` over a pointer c-names as `void *`, not `int64_t`.**
  `(defopaque String :ptr<void>)` used to lower to the same `int64_t` carrier
  word as every other handle, so an opaque handle was byte-indistinguishable
  from a tagged carrier box at the emitter's ~94 `strcmp(cname, "int64_t")`
  sites. It now says what it is. The declared base type had to start being
  recorded for this: `defopaque` parsed it only to find where the trailing
  `:linear` / `:affine` / `:sealed` attributes start, and then threw it away, so
  `:ptr<void>` and `:int` were indistinguishable downstream.

  **Breaking for inline-C over a pointer `defopaque`**, and deliberately loudly
  so: a body that ends `return (int64_t)(intptr_t)p;` in a function returning
  such a handle is now a `-Wint-conversion` (which the suite's ratchet fails on),
  and a hand-written `extern` declaring the handle as `int64_t` is a hard
  `conflicting types`. Both fixes are one line -- return `(void *)(intptr_t)p`,
  declare the parameter `void *`. Stdlib took 66 such edits across 21 files;
  `stdlib/string.tur` took none, because it was already written as
  `(:: (tur_string_adopt_cstr s) String)` over `ptr<void>`-typed externs, which
  is the pattern to copy. Gate results:
  `docs/archive/opaque-pointer-c-spelling-gate-results.md`.

- **SR3's Option niche is unshelved, as `--enable=option-niche`.** The
  0.40.0 entry below shelved it because `String` -- the whole of the phase's
  census -- could not take the niche while it c-named to `int64_t`. The change
  above removes that, so `(Option String)` is carried as its payload pointer:
  16 bytes to 8, `(none)` as NULL, no tag word, and no
  `tur_adt_Option__String` typedef emitted at all. It stays behind a flag
  because "this payload's valid values exclude 0" is a hand-maintained
  allowlist rather than something the type system records; graduating it means
  making non-nullness declarable. `Cons` remains ineligible for the reason the
  0.40.0 entry gives. Plan: `docs/upcoming/sr3-option-niche-plan.md`.

### Fixed

- **`:cmake-deps` link lines are resolved by CMake instead of guessed
  downstream.** `INTERFACE_LINK_LIBRARIES` is now walked recursively inside the
  generated `CMakeLists.txt`, where `if(TARGET ...)` can tell a library name
  (`m` -> `-lm`) from a CMake target name (raylib lists `glfw`) -- identical
  shape, opposite handling, and the C-side guess emitted a `-lglfw` that does
  not exist. An Apple framework arrives as an absolute path to the `.framework`
  *directory* and is now respelled `-framework <name>` rather than passed as a
  link input, which `ld` rejects with `file cannot be mmap()ed, errno=22`.
  Alongside: the walk is scoped to the declared `:spices` closure instead of
  every workspace member (building `spices/opengl` configured 15 unrelated
  native deps, and a single one that could not configure aborted the whole
  build); a transitive `:path` dep is no longer absolutized and then
  re-prefixed; a shared-library dep gets its `-rpath`; `:path` resolves against
  `cmake/`; and CMake 4's policy floor is passed through. A raylib spice now
  builds and runs on macOS with no `cmake-deps/` shim.

- **An inline-C body that builds an `(Option T)` produced the wrong value under
  the niche.** `tur_some_ptr` returns the carrier -- a pointer to a tagged box --
  and a niche consumer read that word as the payload, so
  `(string/to-cstr (unwrap o))` printed blank rather than the string. Silent,
  not a crash. The let-binding and call-argument crossings now bridge through
  `emit_carrier_bridge` like every other. Only reachable with
  `--enable=option-niche`.

- **`: nil` forward declarations no longer collapse to the `int` placeholder.**
  A statement-position call to a `: nil` function the elaborator had not yet
  reached was emitted as `__auto_type __ps_N = <void call>;`, which `cc` rejects
  with "variable has incomplete type 'void'" and no `.tur` attribution. `: void`
  was immune, and moving the callee above the caller made it disappear. The root
  cause was in the elaborator: the reader parses a bare `nil` in type position
  as `F_NIL`, not `F_SYM`, so the forward-declaration pre-passes did not match it.

- **A module-level `def` of an opaque-typed value emits its global.**

- **Two diagnostics now name what they could not find:** a hoisted inline-C
  `#include` that does not resolve names the header, and `tur` says so when
  `TUR_STDLIB_DIR` overrides the stdlib sitting beside the binary.

## [0.40.0] -- 2026-08-28

### Added

- **Try Turmeric navigation (M0-M5, F1).** A minimap with blocks and a
  three-lane overview ruler, gated on measured editor width with a persisted
  override; a Symbols popover fed by the `documentSymbol` provider, sorted by
  position, kind-labelled and caret-tracking; go-to-definition into the stdlib,
  opening a read-only padlocked buffer excluded from downloads, persistence and
  the server's document set, with F12, Cmd+click and a Back button that appears
  only when there is somewhere to go; `documentHighlight` that skips comments,
  strings and inline-C fences; hover that falls back to the documentation table
  when the checker has nothing, marked as docs-sourced; and `builtin_describe`,
  so `println`, `+`, `=` and `not` hover to something at all. A C-interpreter
  link joins the site footer, the `/try` footer and the sidebar's Ecosystem
  list.

- **CI metrics page at `/ci`.** Every push to `main` publishes each ctest
  suite's wall time to the `ci-metrics` branch; the page reads one build
  environment at a time and shows duration trends, per-suite sparklines and the
  skip ledger.

### Changed

- **Parametric sum monomorphs flow by value by default (SR2a graduated).**
  `--enable=parametric-sum-byvalue` is retired -- `(Option int)`,
  `(Result float cstr)` and every other concrete parametric sum monomorph are
  aggregates with no per-ctor malloc, where they used to ride the int64 heap
  carrier. Measured on the seam: 3.6x faster and 71x less peak RSS on a
  narrow-sum loop, 3.2x/145x on a wide one, and one leaked allocation per
  construction eliminated. Shapes the predicate declines (self-recursive,
  `:heap`, GADT, fixpoint partners) and erased generic bases still use the
  carrier. `--enable=parametric-sum-byvalue` remains accepted as a TUR-W0063
  no-op for one minor line; `TUR_SR2_APP_SUM_BYVALUE=0` restores the carrier
  for bisection. Plan: `docs/upcoming/sum-representation-plan.md` (SR2c).

- **`ok?` and `err?` take `(Result A B)`, not `:int`.** They were the last
  carrier-typed Result accessors; `some?`, `ok-val` and `err-val` were already
  parametric. An `:int` parameter stops being a harmless erasure once the value
  flows by value, so inline-C code that hands back a carrier value declared
  `: int` now names its type at the boundary -- `(ok? (:: r (Result int int)))`
  -- exactly as it already did for `some?`.

- **SR3's Option niche is shelved, not shipped.** The representation was gated
  behind `TUR_SR3_OPTION_NICHE=1` (default off) and measured. It works, and one
  erased-crossing bug it exposed is fixed regardless -- a typeclass `Eq`
  dictionary read the low half of a spilled niche pointer as a tag and returned
  a silent wrong answer for two equal `(some v)`. What shelves the phase is the
  population: the niche claims `0` for `None`, so every payload that has already
  spent its null (`Cons`'s `nil` *is* `0`) is ineligible.

### Fixed

- **Seven representation-crossing defects** the by-value default exposed, each
  one a place where two spellings agreed only because a parametric sum monomorph
  and a type variable both c-named to `int64_t`. The sharpest: the
  carrier-to-by-value readback's NULL guard lived in the pre-sum record branch,
  so reading a `(none)` built by inline-C dereferenced the null carrier. Also a
  match on an erased instance base's parameter binding an aggregate from an
  `int64_t` slot, a match resolving its element from a different instantiation
  than the active specialization, an Option/Result pointer-box payload bound as
  a value, an argument spilled to a carrier sink at its static rather than its
  specialized type, a poly-wrapper argument unboxed twice, and the catch-unwind
  group trampoline saving an aggregate-returning member through a scalar cast.

- **CI suites that were passing by not running.** The browser job's desktop step
  runs a fixed spec list, so `minimap.spec.js`, `footer.spec.js` and
  `lsp.spec.js` asserted nothing until they were named (51 tests -> 87). The
  mobile project is WebKit but the job only installed Chromium, so all 32 of its
  tests died at launch behind a `continue-on-error`. Also `tur fmt`'s canonical
  one-line form for the retyped `ok?`/`err?`, and a `docs/reported/README.md`
  row left pointing at an archived report.

## [0.39.0] -- 2026-08-27

### Added

- **Offline documentation, in the browser and out (OD1-OD5).** The guides and
  API pages are rendered once and wrapped twice: the site keeps its chrome, and
  a chrome-free *docs pack* feeds an in-app docs browser in Try Turmeric, so
  reading a guide no longer navigates away from your editor buffer, console
  history, or WASM session. The pack is precached unconditionally on install --
  no toggle, no first-run prompt -- with `#doc=guides/...` deep links, one
  search box over pages and symbols, "Load into editor" on every runnable code
  block, and a remembered scroll position per page. Outside the browser,
  `tur doc <symbol>` answers from the stdlib docstring table rather than just
  the builtin list, and `tur docs [--open|--serve]` locates the rendered
  documentation (`$TUR_DOCS_DIR`, then `<prefix>/share/doc/turmeric`, then
  `<repo>/docs/html`); `--serve` is a loopback-only, GET-only static server for
  the browsers that refuse `file://` navigation.

- **`--enable=parametric-sum-byvalue` (beta).** Parametric sum monomorphs
  (`(Option int)`, `(Result float cstr)`, ...) flow by value with no per-ctor
  malloc, instead of riding the int64 heap carrier. The default path is
  unchanged; the experiment is the staging ground for making by-value the
  default (SR2 graduation). Plan: `docs/upcoming/sr2-gate-results.md`.

- **Lazy solution streams in `stdlib/logic.tur`.** `Stream` gained the immature
  constructor `(StInc :StThunk)` plus `st-force` / `st-pull`, so `run-logic n`
  now costs n solutions instead of running the whole search and truncating the
  result. `st-append` swaps on an immature stream, which is fair interleaving;
  `st-bind` defers through one rather than forcing it. **A relation with
  infinitely many solutions is now expressible at all** -- previously
  `(defn nats [] (disjoined (succeed) (nats)))` did not merely diverge, it
  SIGSEGVed while the goal was being *built*.
- **`zzz`, a delay macro for recursive relations.** `(disjoined (succeed) (zzz
  (nats)))` terminates where the undelayed form crashes. It is a macro by
  necessity: a function would evaluate its argument at the call site, which is
  the divergence it exists to prevent.
- **`disjoined-dfs` / `st-append-dfs` -- depth-first search that is still
  lazy.** `disjoined` interleaves and is complete; this pair keeps depth-first
  order for goals whose left branch is known finite, reaching a solution at
  depth 18 in 0.012s against 3.5s interleaved. Incomplete by construction -- it
  never reaches the right branch of a goal whose left branch is infinite.
- **`stdlib/trail.tur` -- backtrackable state**, behind
  `--enable=backtrackable-state`. Trailed cells whose writes undo to a mark,
  with the opt-out at three granularities: per cell (`g-cell-new`, never
  trailed), per write (`untrailed-begin` / `untrailed-end`), and per level
  (`bt-commit-to!`). `BtCell` and `GCell` are distinct types so opting out is
  visible in a signature. Recording and undoing state measures ~5ns per write,
  roughly 5x cheaper per unit of live state than capturing and restoring
  control.
- **`tur smt <file.smt2>`** runs an SMT-LIB2 script through the refinement
  solver's staged chain and prints `sat` / `unsat` / `unknown`, which stage
  decided, and a model when the bounded search finds one. Exit codes mirror the
  answer (0 unsat, 1 sat, 2 unknown, 3 error) so a shell harness can branch on
  `$?` without parsing stdout.
- **`--dump-refine=json`** emits one record per refinement obligation --
  location, predicate, verdict, deciding stage, counterexample, the replayable
  VC as SMT-LIB2, and which caps bit for that obligation. Works on `check` as
  well as `emit-c`. The schema is explicitly unstable and says so in every
  record (`"schema": 0`).
- **`tests/run-leak-check.sh`** runs compiled fixtures under LeakSanitizer, opt
  in per fixture with a `requires.leak-check` marker. This generalizes three
  bespoke per-regression harnesses; coverage went from 2 fixtures to 54.

### Changed

- **`Option` and `Result` are real sums now** (SR2b,
  `docs/upcoming/sum-representation-plan.md`). `(defdata Option :copy [A]
  (None) (Some A))` and `(defdata Result :copy [A B] (Ok A) (Err B))` replace
  the discriminated records; every stdlib accessor and instance is
  match-based, and you can `match` the variants directly. The runtime layout
  is the tagged monomorph `{ int tag; union { ... } as; }` -- 16 bytes for
  both (`Result` down from 24), tags in declaration order, payload at offset
  8. The dead-arm write is gone, so an error type no longer needs a zero
  value. **Inline-C contract:** hand-rolled `{ bool is_ok; ... }` /
  `{ bool is_some; ... }` structs read the wrong bytes now -- build and read
  through the preamble helpers (`tur_box_ok` / `tur_is_ok` / `tur_box_some` /
  `tur_is_some` / ...), which carry the canonical layout and a
  `_Static_assert` pinning it. The interpreter builds and matches the same
  constructors, with the legacy box shapes still readable.
- **`(none)` allocates nothing** (SR3 slice A). The carrier `None` is the
  null pointer -- every reader already treated NULL as none, so the tagged
  box whose only content was `tag = 0` was pure allocation. A 2e6-iteration
  `(none)` loop peaks at 10.3 MB RSS where the still-boxing `(some i)` twin
  peaks at 64 MB. A tagged None box remains valid on the read side.
- **`Option` and `Result` monomorphs lower by value when a type argument is
  itself a monomorph.** `option<list<int>>`, `result<vec<T>, cstr>` and
  `option<(Pair a b)>` previously fell back to the heap carrier -- a silent
  representation downgrade that reintroduced a `malloc` per construction on some
  of the most common shapes in the language.
- **`NO_MAX_SHARED` raised 8 -> 16.** It was the only cap in the refinement
  solver with a live signal, turning away eligible terms on four units and
  always by exactly one. No verdict moved on the 125-benchmark corpus or the 89
  in-tree refinement fixtures, and the corpus replay did not slow measurably.
- **`-main` is no longer documented as an entry point.** Nothing ever called it:
  both shipped examples and the snake tutorial used it, and `examples/minikanren`
  built, linked, ran, exited 0 and printed nothing. Both examples and all 24 of
  the tutorial's listings now use `main`.

### Removed

- **Twelve graduated `--enable=` compatibility shims retired.** A `GRADUATED[]`
  entry is a migration window, not a permanent alias: a name ages out one minor
  line after graduation and goes back to being the hard `TUR-E0310` an unknown
  name gets. Five backend names went first (`cps-effects`, `cps-tramp-resume`,
  `cps-async`, `owning-cloneable-capture`, `closure-drop-glue`) -- no source
  syntax to adopt, so nobody had reason to name one in a build. Then the seven
  that gated source syntax and had each had a full minor line: `refined`,
  `cycle-gc`, `jit`, `sealed-opaque`, `global-state`, `write-frames`,
  `checked-reads`. `#lang turmeric refined` is `TUR-E0330` again -- a semantic
  layer *is* its experiment, so the two shims retire together. `jit-ffi`
  graduated at 0.38.0, so its window opens now and it is the sole survivor.
  **If a `build.tur` or `experiments.tur` still names a retired flag, drop the
  name** -- the feature it gated needs no enable at all.

### Fixed

- **Statement position deleted wrapped expressions -- a high-severity
  miscompile.** `emit_stmt` treats four pure *wrappers* (`EX_REINTERPRET`,
  `EX_CAST`, `EX_ASCRIBE`, `EX_POLY_WRAP`) as pure in statement position and
  emitted nothing for them. The wrapper is pure; the call inside it is not, so a
  discarded parametric call -- whose result rides the int64 carrier and is
  therefore wrapped by elaboration to restore its instantiation type -- vanished
  along with its effects.
- **The aarch64 HFA ABI is correct in the JIT.** AAPCS64 passes a struct or
  array of 1-4 same-typed FP members in `v0..v7`, one per register; MIR's
  aarch64 back end had no HFA concept and routed every aggregate through the
  integer argument registers. Self-consistent inside one c2mir compilation and
  wrong the instant c2mir code met natively compiled code. The MIR pin moves to
  the upstream fix, and both interim refusals it forced are dropped. The
  compiled `tur jit` path, which had no check at all, miscalled an ordinary
  `extern-c` with a record parameter; it now refuses (`TUR-E0711`) where the fix
  does not reach.
- **One convention for wide by-value aggregates at every fat boundary.** A
  lifted thunk and its dispatch site could disagree about how a >8-byte
  by-value aggregate parameter crosses a fat-closure boundary: a hard `cc` error
  through a `^fat` sink, a **silent wrong answer** through the untyped `:fn`
  carrier, and a SIGSEGV through a typed fn-field, whose cast is exactly what
  hid the disagreement from the C compiler.
- **`tur fmt` silently deleted comments inside bracket vectors.** Every `;` /
  `;;` / `;;;` comment inside a `defstruct` field vector, a `defn`/`fn`
  parameter vector, or a `let`/`loop` binding vector was dropped in place, exit
  0, no diagnostic. The non-idempotence (`tur fmt` then `tur fmt --check`
  exiting 1 on the file `fmt` just wrote) was the downstream symptom.
- **`(fn name [...])` points at `letrec` instead of a bracket error.** The
  Scheme/Racket/CL spelling of a self-recursive lambda was rejected with
  "parameter list must be a vector", caret on the name, with a well-formed
  vector sitting one token later -- a message that invited the reading that
  Turmeric has no recursive lambdas. It has two, and the diagnostic now names
  both.
- **Two modules could silently share one API page.** `just docs` wrote 147 pages
  for 148 modules: the filename-derived fallback name keys on the *basename*, so
  `stdlib/capability.tur` and `stdlib/test/capability.tur` both rendered to
  `tur-capability.html` -- the shipped page held the test mocks and the real
  module had none, while the index still showed both cards.
- **`vec-of` over a parametric sum monomorph no longer ICEs.**
  `(vec-of (Yep 8) ...)` over a two-variant sum died at the let binder on the
  default path (the Vec registration and the binder disagreed about the
  element's representation); `vec<option<T>>` is that shape. Fixed by the
  SR2b representation predicates; the report is archived.
- **`rc/of` did not release a multi-variant ADT payload.** It allocated a second
  box for the carrier word and freed only that wrapper, so the payload leaked --
  code doing exactly the documented thing lost 16 bytes per value.
- **`ref/from-rc` and `(upgrade w)` leaked at the ownership handoff.** Two
  unrelated call sites, one shape: a heap allocation handed across an ownership
  boundary to something that never freed it.
- **A closure stored in an ADT field emitted C that warned.** A `defopaque`
  over `:ptr<void>` is a named `int64_t` carrier, and a closure ascribed to one
  still lowers to a pointer, so the store was an int/pointer straddle the
  emitted C complained about. This is what any ADT holding a callback hits.
- **`fat_captures_borrowed` was read out of uninitialized memory.** The flag
  suppresses one specific use-after-free; 60 fixtures were reading it as garbage
  on every compile, and nothing failed because UBSan here prints and continues
  rather than aborting.

### Docs

- **Eight dead guide cross-links fixed, and `--strict-links` armed** so the next
  one fails the docs build instead of shipping a 404 to turmeric-lang.com.
- **The logic guide no longer tells you to hand-write an interleaving `mplus`.**
  `st-append` is the interleaving one now, and the section explains why the
  hand-written version it used to recommend could never have worked: it swapped
  its arguments but built a strict cell, and interleaving without an immature
  result is half a mechanism.
- **The test-suite portability guide gained a section on what the sanitizers
  actually catch** -- ASan aborts, UBSan does not, and the suite now collects
  UBSan findings from the compiler and reports them after the summary
  (`TUR_SANITIZER_GATE=1` makes them fatal).

## [0.38.0] -- 2026-08-21

### Added

- **Dynamic FFI carries by-value records in every direction.** `call-ptr` and
  `callback-ptr` take and return records (including nested ones), a callback can
  receive and return aggregates, and `extern-c` gained by-value record
  parameters and returns. Under `--interpret` this routes through the c2mir
  thunk provider and needs a `-DTUR_JIT=ON` build; compiled code needs nothing.
- **Nested constructor patterns in `match` arms**, and `#json-str?<T>` -- a
  `Result`-returning typed JSON decode.
- **`:global` spice dependencies** -- consume a globally installed spice as a
  library.
- **`mw-recover` in httpd**, and a panic boundary around every task: a panic
  inside an `(async ...)` body no longer unwinds the spawner and aborts the
  process; `await` re-raises it instead.

### Changed

- **`jit-ffi` graduated: `call-ptr` and `callback-ptr` need no `--enable`.**
  They are ordinary `unsafe` forms (a lingering `--enable=jit-ffi` is a
  `TUR-W0063` no-op). The gate existed only so the signature vocabulary could
  move; that vocabulary is now settled and measured. The `-DTUR_JIT=ON` build
  gate on the interpreter path is unchanged, and `unsafe` is still required.
  `EXPERIMENTS[]` is now empty.
- **`stdlib/args` names real handle types.** `ArgSpec` / `ArgResult` are
  `defopaque` instead of bare `:int` across all 18 entry points, and an option
  default is `(Option cstr)` rather than a `cstr` smuggled through `:int`.
- **Float division no longer emits the integer divide-by-zero guard**, so IEEE
  inf/NaN semantics survive instead of trapping.

### Fixed

- **`(cast a OtherStruct)` on an `any` succeeded and reinterpreted the
  payload.** Every struct boxed as `TY_STRUCT` and every ADT as `TY_ADT`, so the
  tag check compared equal between unrelated types, and `type-of` answered
  "struct" / "adt" for all of them. A struct/ADT payload now interns its own
  type.
- **A narrow C return value was read as garbage.** A callee returning `int`
  leaves the upper half of the return register unspecified, so a thunk declared
  `long long` read whatever was there -- `neg_int(1234)` came back as
  `4294966062`. The FFI signature vocabulary is exact-width now.
- **Nested by-value record fields were marshalled to the wrong shape under
  `--interpret`**, on every architecture -- `{{ww}w}` passed as `{qw}`, silently
  wrong answers. Found by the x86-64 verification that had never been run.
- **`match` binds the variable of an ADT catch-all arm.**
- **`catch` keeps the payload's representation on both engines**, including an
  aggregate-returning thunk and a float payload read from the erased `Result`
  carrier.
- Codegen: a divergent-tail function's trailing `return`, inline C naming a
  local, forward-declared globals a lifted lambda reads, and the fn-typed
  if-merge temp.

### Docs

- Every example that compiles is now also run in CI, and the ECS benchmark
  landed.

## [0.37.0] -- 2026-08-20

### Added

- **`tur audit` -- where this build fetches code from.** Reads `build.tur` plus
  `tur.lock` and prints every origin, spices and `:cmake-deps` in separate
  sections, with URL, ref, subdir, and the resolved commit and SHA-256 where
  the lock has pinned it. Unpinned origins are called out with the fix. It
  verifies nothing, and says so on every run.
- **A concurrency stdlib layer**: `arc.tur` (the language surface over the Arc
  runtime), `barrier.tur` (a reusable counting barrier), `stm-sync.tur`
  (`TMVar` and `TChan` over `tvar` + `check`), and `with-lock` /
  `with-read-lock` / `with-write-lock`.
- **`(export-from <mod> name ...)`** -- re-export a name from another module
  without importing it locally.
- **`:entry` in `build.tur`**, `#map{}:(K V)` typed-empty map literals, and
  `cstr-eq?` / `cstr-free` in `stdlib/cstr`.
- **A verification tier for `#reads` frames.** A deferred footprint walk
  reports VERIFIED / EXCEEDED / UNVERIFIED per frame via `--dump-read-frames`,
  and an EXCEEDED reached through a callee's own frame -- a read the
  definition-site scans cannot see -- joins the `TUR-W0383` evidence tier.

### Changed

- **`write-frames` graduated: a `#writes` frame is checked without
  `--enable`.** WF2's three verdicts (VERIFIED, EXCEEDED -> `TUR-E0382`,
  silent UNVERIFIED) and WF3's borrow widening are now unconditional. WF4's
  entry-check elision was retired before graduation -- the check it proposed
  to elide does not exist -- so what graduated is a checker that reports a
  broken promise, not an optimization acting on one.
- **`checked-reads` graduated: a broken `#reads` frame no longer buys a
  proof.** When a measure's body demonstrably reads mutable state its frame
  omits, the congruence override is refused and the crossing becomes the
  ordinary `TUR-W0372` (a hard error under `--strict-refine`). Refusal keys on
  "saw a read", never "could not see", so an inline-C measure -- essentially
  every measure predating mutable globals -- is unaffected. Note what this does
  not do: a `#reads` crossing is proof-only, so refusing buys a diagnostic
  rather than a check, and outside `--strict-refine` the program still runs.
- **`schan-recv` returns `(Pair T (SChan R))`** instead of writing through an
  out-parameter.
- **An int literal ascribed to a float is the number, not its bits.**
  `(:: 3 :float)` printed `1.4822e-323` -- the double whose bit pattern is
  `0x3` -- and now folds to `3.0`. The tell that this was an accident rather
  than a semantic: `(:: 3 :float32)` already printed `3`, because the
  same-width reinterpret rule missed at 8 != 4.
- **The Send-across-await check runs at every await point**, not just the
  first.

### Fixed

- **A SIGSEGV passing a let-bound non-capturing lambda as a `:fn` argument.**
- **`TUR-W0033` fired on the very `(unsafe ...)` block it requires.**
- **`tur run test` now reaches ctest** and passes 108/108.

### Docs

- **A repo-wide documentation accuracy pass.** The guides that had drifted from
  the shipping API were rewritten against it (performance, logic,
  checkpointing, the quickstart tutorial, the datalog examples), and 33 reports
  were filed for what could not be fixed in place.

## [0.36.0] -- 2026-08-19

### Added

- **`defmacro*` -- procedural macros that run on a macro-time interpreter
  environment.** A macro body is ordinary Turmeric evaluated at expansion time
  over a first-class `Syntax` value (a new `TY_SYNTAX` compile-time kind, syntax
  natives, and quasiquote that produces `Syntax`), with the stdlib preloaded
  into the macro env; the derive-family migrated onto it as proof.
- **`(import m :for-macros)` -- cross-module macro-time dependencies.** A module
  imported for macros is loaded into the expansion env rather than the runtime
  one, so a procedural macro can call helpers defined elsewhere. Macro-time I/O
  is denied by default.
- **Procedural reader macros by composition, plus R3-bounded reflection** -- a
  reader macro is an ordinary `defmacro*` composed into the read step.
- **`tur expand` and REPL `:expand`** -- one expansion step at the command line
  and at the prompt. `defmacro` bodies may now hold multiple forms, and gensym
  is unified across the expander.
- **jit-ffi F4/F5 -- struct-by-value through `call-ptr`, and callbacks.** A
  `(unsafe (call-ptr ...))` signature can now pass and return structs by value,
  and a Turmeric function can be handed to C as a callback on both the compiled
  and the interpreted path. The aarch64 FP-aggregate ABI wall F4 hit is
  reported in `docs/reported/mir-aarch64-fp-aggregate-abi.md`.
- **`tur completion <zsh|bash>` -- shell completion.** Completes subcommands,
  per-subcommand flags, and `.tur` file arguments; for `tur run` it completes
  recipe names out of whatever Justfile encloses the directory being completed,
  using their doc comments as descriptions. The scripts are embedded in the
  binary, so `source <(tur completion zsh)` bootstraps anywhere with no
  install-prefix lookup. The Homebrew formula installs both.
- **`tur run --list --all`** shows recipes that are normally hidden.

### Changed

- **`#reads` may name multiple parameters** (`#reads [a b]`), and a mutable
  global is never frozen by a `#reads` frame -- a callee's write to a global the
  frame named no longer survives as an unearned congruence grant (soundness).
- **`[private]` and `_`-prefixed recipes are honored rather than rejected**,
  matching `just`: hidden from `tur run --list`, still runnable by name.

### Fixed

- **Two Result box/struct bridging codegen bugs** -- a CPS-path Result unbox was
  dropped, and a by-value product tail in a `result` block was double-unboxed.
  Every parametric by-value product tail is now covered; the remaining
  non-parametric shape is reported rather than miscompiled.
- **A heap join whose body escapes to an enclosing join** emitted an invalid
  assignment; it is now evicted to the direct emitter.
- **`__TUR_CNAME_` broke on leading underscores**, and a `let` binding of a
  `:void` expression emitted invalid C -- now a clean TUR-E0023.
- **Malformed or duplicated `#reads` frames** are diagnosed (TUR-E0024).
- **`tur run --list` omitted aliases.** `alias b := build` is runnable --
  `find_recipe` resolves it, and the "recipe not found" error even printed
  aliases in its `available:` line -- but the listing walked only the recipe
  table, so aliases were invisible to any tooling built on it.
- **One unsupported Justfile feature blanked the whole listing.** A `[private]`
  recipe or a `mod` line -- both fine under real `just` -- aborted the parse
  with exit 2 and no output. Unsupported features now degrade `--list` (note on
  stderr, remaining recipes still listed) while staying fatal when a recipe is
  actually executed.
- **`tur run --list --json` escaped only `doc`.** Recipe names and parameter
  defaults were emitted raw and control characters passed through, so a default
  like `flags='-DFOO="bar"'` produced invalid JSON.

## [0.35.0] -- 2026-08-18

### Added

- **`(unsafe (call-ptr p [T1 T2 -> R] args...))` -- call a raw function pointer
  through a JIT-compiled thunk**, behind `--enable=jit-ffi`. The thunk is
  rendered from the signature string, compiled through c2mir, and cached per
  unique signature, so a JIT build calls a C function of any arity with no
  `--max-arity` ceiling. It is not a new expression kind -- the signature hangs
  off the ordinary call node, so every walker traverses it unchanged. F1-F3 of
  docs/upcoming/jit-ffi-c2mir-plan.md; struct-by-value (F4) and callbacks (F5)
  deliberately trail.
- **`extern-c` stops lying under `--interpret`.** In a JIT build an `extern-c`
  registration resolves the symbol via `dlsym` and binds a thunk-backed native,
  so `(strtol "123abc" 0 10)` is `123` where everything outside a 7-entry known
  table used to silently return nil. The known table stays as the
  semantics-bearing override (`free` no-op, `exit`, `printf` marshalling).
- **A dialect picker and layer toggles in Try Turmeric.** The editor header
  gains a Language control -- a radio group for the four base dialects and
  checkboxes for the curated `#lang` layers -- rendered from a new WASM registry
  export so the UI never becomes a second source of truth. The `#lang` line
  stays authoritative: typing it by hand and using the picker are the same
  operation, and one Ctrl+Z undoes a switch. Turning a reader layer off now
  genuinely deactivates its `#`-dispatch, where `#s"..."` used to keep reading
  as `String` after `stringed` was switched off.
- **Variadic spice exports are callable from the REPL**, and a spice that
  declares its C dependency the recommended way (`:cmake-deps` / `:link-libs`,
  no `__tur_autolink__` marker) now loads through the REPL's in-process JIT
  hook, falling back to the subprocess build when the hook cannot handle it
  (vendored `:c-sources`, static-only cmake deps).
- **Rust and Haskell benchmark columns, and `tur jit --timing-json`.** 21
  programs each, validated byte-for-byte against the existing goldens, plus a
  phase record (`compile_ms` / `run_ms` / `engine`) so a chart can subtract
  compile time and a `cc` fallback is detected rather than averaged in. The
  existing language list is unchanged; the new columns ride on top.

### Changed

- **`global-state` graduated -- four mutable-global features work without a
  flag.** A `#writes` frame may name a mutable global, an exported global is
  read-only outside its defining module (write it from another module and you
  get a diagnostic naming the owner and `(export (mut g))`), and `^atomic` /
  `^thread-local` are ordinary annotations on a top-level `def`. Every phase of
  docs/upcoming/mutable-globals-plan.md had landed, so the row had nothing left
  to decide. A lingering `--enable=global-state` is a `TUR-W0063` no-op for one
  minor line, not an error.

  One tightening rides along, and it is confined to `--enable=write-frames`
  (still experimental, and what *checks* a frame at all): a body that declares a
  frame and writes a global the frame does not name is now `TUR-E0382`, where it
  previously just declined to verify.

### Fixed

- **The macOS CI 45-minute hang.** `httpd-stop-async` left the listen fd open
  until `httpd-async-free`, so the kernel kept completing handshakes into the
  backlog after the stop; a client that got one blocked forever in `recv()`,
  deadlocking `main` in `pthread_join`. Both stop paths now close the listener,
  so pending backlog connections are reset and late connects refused rather than
  black-holed. Reproduced 100% by delaying one client 300ms, 0% after, and
  stressed 200 runs at 6x thread oversubscription.
- **`kqueue` write knotes were never deleted.** `EV_DELETE` passed
  `EVFILT_READ | EVFILT_WRITE`, but kqueue filters are enum values (-1, -2), not
  a bitmask, so the OR collapsed to `EVFILT_READ` and a stale WRITE knote could
  deliver a wake on a reused fd.
- **`:build-opts :link-libs` reaches the link line.** It was parsed, documented,
  and round-tripped by `tur init` -- and consumed by nothing.

### Docs

- **A dynamic FFI guide**, with a worked `libzmq` example, plus a plan for
  spice-level FFI integration.

## [0.34.0] -- 2026-08-17

### Added

- **`TUR-W0383`: a `#reads` frame that omits mutable state the body reads now
  warns at the definition.** `#reads` is trusted, and its one consumer grants
  congruence -- so a measure declared `#reads w` whose body also reads a
  mutable global was silently buying proofs it had not earned (the elided
  caller-side crossing check the `refine-reads-frame-omits-global` fixture
  pair pins). The warning is gateless and changes nothing proved: it reports
  positive evidence of the broken promise (a direct read of a `^mut` global in
  the elaborated body) without yet refusing the override. An inline-C body
  yields no evidence and stays silent, so every pre-existing measure is
  unaffected. `tur --explain TUR-W0383` has the full story.
- **`--enable=checked-reads`: refuse the `#reads` congruence override on
  broken-frame evidence.** The gated escalation of TUR-W0383: on the same
  positive evidence (the measure's body directly reads a mutable global), the
  refinement encoder declines the congruence grant, so a crossing that used to
  be proved from the broken promise becomes an undischarged TUR-W0372 -- with
  wording that says the *frame* failed, not the region ("fix the frame, not
  the region"), since the usual "guard it inside a `frozen` region" advice is
  misleading when the region is present. A hard error under `--strict-refine`.
  Refusal keys on "saw a read", never "could not see": an inline-C measure --
  essentially every measure that predates mutable globals -- carries no
  evidence and keeps today's trusted behavior even with the gate on. R2 of
  docs/upcoming/trusted-refinement-claims-plan.md.
- **An execution engine can be selected per project.** `:engine "cc" | "jit" |
  "interp"` in `build.tur`, `--engine <name>` on the command line, or
  `TUR_ENGINE` in the environment, resolved in that precedence with `"cc"`
  last -- the same ladder `:build-dir` already used. `tur init` round-trips the
  key. There is **no silent substitution**: an unknown value is a hard error
  (`TUR-E0311`, with its own `tur explain` entry) and asking for `"jit"` on a
  build without the engine names `-DTUR_JIT=ON` and the override spellings
  rather than quietly falling back, because the engines differ in *semantics*
  and not just speed. Unknown manifest *keys* are still silently ignored, which
  is the documented compatibility story.
- **`tur repl --engine <name>`.** Selects the engine that builds the enclosing
  spice: `"cc"` (the `tur build --shared` subprocess, still the default) or
  `"jit"` (compile the whole spice in process through MIR, no `.so`, no
  `dlopen`). Reads the same precedence ladder as above, so `TUR_ENGINE=jit` or
  `:engine "jit"` in `build.tur` work too.
- **An error inside macro-generated code now names the call that generated
  it.** Template spans survive expansion, so a diagnostic used to point into
  the `defmacro` body with nothing tying it to the code the user actually
  wrote. One note is appended at the call site -- "in expansion of macro
  'name' -- the diagnostics above are inside code this call generated" -- on the
  outermost frame only, so nested macros get a single note at the user-visible
  call and a clean expansion followed by an unrelated error gets none.
- **`maximum macro expansion depth exceeded` carries a hint** naming the two
  measured causes of a base case that never fires: `nil?` on an empty `^syntax`
  rest (an empty rest is an empty *list*, so `empty?` is the predicate), and
  counting-driven recursion (the compile-time evaluator has no arithmetic, so a
  spliced `(- n 1)` recurses on the unevaluated form).
- **`^thread-local` on a top-level `def`**, behind `--enable=global-state`.
  Each thread gets its own copy, materialized on first access and initialized by
  running the declared initializer *on that thread* -- so
  `(def ^thread-local buf (make-buf))` gives each thread its own buffer rather
  than sharing one. Lowered to a `pthread_key_t` holding one per-thread block,
  not `__thread`: C has no dynamic thread-local initialization, and the JIT's
  c2mir has no thread-local storage at all (it would silently share one slot
  across threads). The key's destructor frees the block on thread exit. Under
  `tur --interpret` it is a plain global -- turi has no user-reachable thread
  spawn, so there is no second thread for it to differ on. Does not combine with
  `^atomic`, and its initializer may not reference another `^thread-local`.
- **`^atomic` on a top-level `def`**, behind `--enable=global-state`. Every read
  of a `^atomic ^mut` global lowers to a sequentially-consistent load and every
  `set!` to a sequentially-consistent store. The practical benefit is as much
  the *load*: a bare global read in a loop may be cached in a register, so a
  spinning reader would never observe another thread's store however atomically
  it was made. **It does not make `(set! c (+ c 1))` safe** -- that is a load
  then a store, not an atomic read-modify-write, and two threads still lose
  updates; use `stdlib/atomic.tur`'s CAS/fetch-add or a lock. Eight-byte scalars
  only (`:int`, `:float`, `:cstr`, `:ptr`); anything else is refused with a
  reason. `^atomic` does not imply `^mut`.
- **An exported global is read-only outside its defining module**, behind
  `--enable=global-state`. A module that exports a counter for reading no longer
  thereby exports it for writing; `set!` on another module's global names the
  owning module and both ways out. The permission is granted at the definition
  site with `(export (mut g))` -- reusing the structured-export form
  `(export (effect Name))` established, rather than adding an annotation -- so
  the decision sits with the code that owns the invariant. Only bites across a
  real module boundary: single-file programs and in-module writes are
  untouched. `(mut ...)` on a function or an immutable global is rejected by
  name rather than left inert.
- **A `#writes` frame may name a mutable global**, behind
  `--enable=global-state`. `(defn bump! [] #writes [hits] : void ...)` lets a
  body that maintains global state carry a *checked* frame instead of being
  declined outright, and a frame may mix the two (`#writes [a hits]`). Coverage
  works as it does for parameters: writing a global the frame does not name is
  `TUR-E0382` naming the global, declared-but-unwritten is fine (a frame is an
  upper bound), and an unresolvable body is UNVERIFIED. Naming an immutable
  global is `TUR-E0381` with its own reason. Deliberately `#writes` only --
  `#reads` grants congruence, so a global there would let a promise about
  mutable global state pay out in proofs.
- **`--dump-write-frames`** prints the checked verdict for every declared
  `#writes` frame, with the frame's own verdict and the global-write answer as
  separate columns (`frame=VERIFIED global=YES`). A diagnostic knob, not an
  experiment: it reports what the checker decided and changes nothing.

- **`^mut` on a top-level `def` -- mutable globals.** `(def ^mut hits 0)` gives
  static storage that `set!` may write. This closes a dead end: `set!` on a
  global already advised "use `^mut` at the binding site", and the binding site
  rejected `^mut`, so the only fix the diagnostic named did not exist. Without
  the annotation a global stays immutable and `set!` on it is still an error.
  A `^mut` global is process-wide mutable state with no synchronization --
  nothing checks that it is shared safely across threads.
- **`def` annotations may appear in any order**, matching `let`:
  `(def ^mut ^persistent c ...)` and `(def ^persistent ^mut c ...)` are the
  same declaration.

### Changed

- **Three experiments graduated: `cycle-gc`, `sealed-opaque`, and `jit`.** Each
  gate had nothing left to decide. Existing opt-ins keep working and can be
  deleted at leisure -- `--enable=<name>`, `:experiments [<name>]` in
  `build.tur`, and the user experiments file are all accepted as no-ops with
  `TUR-W0063`.

  - **`(gc-auto!)` is an ordinary call form.** What graduated is the *call*, not
    a default. `GC_AUTO` remains strictly opt-in, permanently: a program that
    never calls `(gc-auto!)` still runs the pure-RC path with no collector
    overhead, and the AUTO-only allocation cost is conditional on the mode at
    run time. **Automatic GC is not becoming the default in this language**,
    before or after v1. That the two decisions were separable is the whole
    reason ungating cost a non-calling program nothing. Baked from 0.30.8 across
    the 0.31-0.33 lines, with pause time fixed, the allocation-path cost
    measured (~10%, fixed overhead rather than per-byte), and steady-state
    residue on a real-shape workload measured at ~60 blocks.
  - **`:sealed` on a `defopaque` now enforces on every compile.** Outside the
    declaring module, `::` refuses both the unwrap and the fabricate direction
    (`TUR-E0302`), closing the extract-and-re-wrap aliasing hole that otherwise
    bounds every guarantee built on an opaque handle. Unusually low-risk for a
    graduation: with the gate off `:sealed` already parsed and imposed nothing,
    so this reaches only code that deliberately *wrote* `:sealed`. The
    two-direction rule the gate existed to question survived the one spice that
    adopted it, so it graduates as designed rather than narrowed to
    fabrication-only. Still a compile-time discipline over the `::` surface --
    inline-C can cast an `int64_t` to anything, so this raises the bypass from
    "one `::` away in ordinary code" to "requires deliberate inline-C" and does
    not claim more.
  - **`tur jit` no longer needs a run-time flag.** The parity condition the gate
    was holding for is discharged: the whole fixture corpus runs through the
    engine on both hosts with an empty denylist. **The build-time gate stays and
    is now the only one** -- `-DTUR_JIT=ON` vendors MIR at configure time, a
    default build carries neither the fetch nor the dependency, and `tur jit` on
    such a binary says so. `cc` is still the default engine; the JIT runs when
    you invoke `tur jit` or when engine selection asks for it.

  One thing moved rather than being deleted: `--enable=jit` was the only switch
  that turned on the in-process REPL spice loader, so removing it would have
  forced a choice between making that path the default and losing it. It now
  hangs off engine selection (`tur repl --engine jit`), which is why `tur repl`
  grew `--engine` in this release. Unset, the subprocess path is unchanged.
- **A capturing closure passed to an effect-row'd `(fn ...)` parameter is now
  `TUR-E0007`.** Such a parameter keeps the thin calling convention, which has
  nowhere to carry a closure environment, so the callee jumped into the
  environment box as code -- a clean compile and a SIGBUS at run time. Only the
  capturing, non-performing shape reached the crash (a capturing callback that
  *performs* already died loudly at CPS-subset eviction). Concrete, empty, and
  row-variable rows all rode thin and all crashed; all three are now refused at
  the call site. Annotating the parameter `^fat` is the way to accept one.
- **`tur build <dir>`, `tur check <dir>` and `tur test <dir>` walk
  subdirectories.** They used a flat `readdir`, so a spice whose modules live
  one level down -- `src/demo/lib.tur`, the layout `:exports "demo/lib"` implies
  -- reported `no .tur files found in 'src/'`. That was the exact invocation the
  `module not found` diagnostic recommends, so the advertised recovery from one
  confusing error produced a second one. Project mode already recursed, so the
  two spellings of "build this spice" disagreed. `<dir>` also goes on the
  include path as its own module root now, without which finding the files
  merely moved the failure to `module 'demo/lib' not found`.
- **`def` and `define` are one form; position, not spelling, selects the
  meaning.** `def` at the top level is a global binding (unchanged, and
  redefining is still an error); `def` in a body is a binding scoped over the
  rest of the body -- what `define` has always done. `define` is an accepted
  alias for `def` in both positions. Nothing that compiled before compiles
  differently: every change is a position or spelling that used to be an error
  becoming legal. See
  [docs/archive/def-define-consolidation-plan.md](docs/archive/def-define-consolidation-plan.md).
- **A name defined at the REPL prompt with `define` now survives to the next
  turn.** `define` used to error at the top level, so the REPL worked around it
  by wrapping each turn containing one in an implicit `(do ...)` -- which also
  scoped the binding to that single turn. A top-level `define` is a real
  top-level binding now, so the wrap is gone. Anyone relying on the
  turn-scoped behaviour was relying on a workaround.
- **A body binding takes a `: type` ascription**, in either the spaced
  (`(def x : float 7.1)`) or fused (`(def x :float 7.1)`) spelling, matching
  top-level `def` and `let`. It used to be an error.
- **A `def`/`define` in an expression position gets an explanation instead of a
  rule.** `(if c (def x 1) ...)` now says the binding has nothing to scope
  over, and names the positions that would work.

### Fixed

- **A runaway macro on a sanitizer-instrumented (Debug) build now reports
  `maximum macro expansion depth exceeded` instead of aborting with an ASan
  stack-overflow.** The 256-level depth counter is a proxy for stack
  headroom, and ASan's redzone-inflated frames could exhaust the real stack
  first (observed on macOS/arm64 Debug; reproducible anywhere with
  `ulimit -s 4096`).  The guard now also measures the thread's actual
  remaining stack (glibc/macOS/Windows queries; the SP register is read
  directly because ASan's fake stack makes local addresses useless for this)
  and raises the same diagnostic pair -- plus a note naming the early stop --
  when headroom is nearly gone
  (docs/archive/macro-depth-guard-loses-race-with-asan-stack.md).
- **`tur emit-c` output now links at `-O0`.** The dead base generic thunk
  chain (a generic fn returning a closure over a type application, e.g.
  `(fn [] (Cons A))`) referenced the base `ctor_X` of a heap parametric ADT,
  a symbol that is never defined -- only per-spec monomorphs are.  `-O2`
  dead-stripped the chain, but a hand `-O0` compile of `emit-c` output died
  with `undefined reference to ctor_Cons`.  The emitter now flushes static
  trap stand-ins (fprintf + abort naming the ctor) for those never-defined
  base ctors into the forward-decl band, covering the n-arg and 0-arg ctor
  branches and both drivers (whole-program and per-TU), so the emitted C is
  self-contained at any -O level.  A genuinely live base-ctor call -- a
  compiler defect, previously an unconditional link error -- now aborts
  loudly at runtime instead
  (docs/archive/dead-base-thunk-chain-references-undefined-ctor.md).
- **A dynamic variable's `pthread_key_create` failure now aborts with a
  message instead of being ignored.** On `EAGAIN` (the process key budget --
  `PTHREAD_KEYS_MAX`, 1024 on glibc, one key per `defdynamic` plus one shared
  by every `^thread-local` -- is exhausted) the key was left uninitialized and
  every later `pthread_getspecific` on it was undefined behaviour: a silent
  wrong-value failure. The emitted `_dynvar_init_*` now checks and aborts,
  mirroring what `^thread-local`'s key init already did
  (docs/upcoming/mutable-globals-plan.md section 13.3).
- **The rational/complex numeric tower now runs on all three engines.** Measured
  under the MIR engine for the first time: every rational and complex fixture
  passes with **zero** `cc` fallbacks, from one pure-Turmeric implementation
  against the same expected output. The 16-byte-struct ABI problem this was
  expected to hit never materialized, so the by-pointer workaround sketched for
  it was never needed. The standing rule that no `_Complex`, `<complex.h>`, or
  `__mul*c3`/`__div*c3` reaches the generated C is now enforced by every plain
  `bash tests/run.sh`, not only by its own ctest target.
- **The interpreter resolves typeclass dictionaries the same way the compiler
  does.** All three recovery heuristics turi used to guess an instance from a
  receiver's runtime tag are retired, the last one covering an unascribed
  carrier-helper read inside a constrained container instance. Where the
  compiled path had solved these statically, the interpreter was pattern-matching
  on runtime tags and could disagree with it; dictionary passing now carries all
  of them, so the two engines agree by construction rather than by coincidence.
- **Several float and carrier miscompiles.** A method result declared `:float`
  keyed off the *body* rather than the declared result type; a float literal
  ascribed to a narrower float width was not retyped in place; a `float32`
  generic-call result had no admitted carrier pair; and the int-slot/float-body
  engine divergence (`TUR-E0707`) was asymmetric between engines. Also a silent
  per-push leak in the container-element path, and seven mismatched fat-closure
  function-pointer types.
- **A multi-shot resume across a nested handler delivers once.** The handler
  chain carried two separate spines, so a resume crossing a nested `handle`
  could deliver twice or not at all. `while` loops and statement-position
  conditionals inside a handler clause work now too -- the latter used to hit an
  internal compiler error rather than a diagnostic.
- **A malformed `build.tur` fails the command instead of vanishing.** A manifest
  that failed to parse was treated as absent, so the command proceeded with
  whatever defaults applied and the real problem never surfaced.
- **Assorted correctness fixes.** A `^borrow` parameter passed where `^unique
  ^mut` is required is rejected; an explicit `: nil` return on a lambda is
  honoured; generic instance resolution survives a `#lang` switch; an HKT type
  variable is pinned from the argument type across a rank-2 `forall`; a return
  type disagreeing with an aggregate body is rejected; `definstance` constraint
  types resolve through the real type resolver; `println` prefers a resolved
  `bool` shape over the runtime tag; a hoisted inline-C include no longer
  disables the JIT's split-preamble fast path; the REPL's source-file registry
  survives incremental eval turns; elaborator-minted names stay out of the LSP
  symbol index; and `term/set-cooked` restores the saved terminal mode rather
  than a zeroed one.
- **A `#writes` frame is no longer VERIFIED when the body writes a mutable
  global** (behind `--enable=write-frames`). A frame's vocabulary is
  *parameters*; a global is written by name rather than passed, so `#writes []`
  means "writes none of my arguments", not "writes no storage anywhere" -- and
  a body declaring it could mutate global state and still be stamped VERIFIED,
  which is the tier an optimization may act on. The verdict now downgrades to
  UNVERIFIED, silently: a global write is outside the frame's vocabulary rather
  than outside the declared frame, so no program stops compiling and no
  diagnostic is added. The fact propagates through callees, including callees
  that receive none of the caller's parameters. An EXCEEDED frame is still
  reported -- a global write does not launder TUR-E0382. See
  [docs/upcoming/mutable-globals-plan.md](docs/upcoming/mutable-globals-plan.md).
- **Statements above the first body-level `define` are no longer silently
  dropped.** The define splice built its `let` from the first `define` onward
  and discarded everything before it, so in
  `(defn main [] : int (println "before") (define x 1) (println x) 0)` the
  first `println` never ran and the program printed only `1`. It now prints
  both lines.
- **`^persistent` and `^deprecated` on a body binding are rejected rather than
  quietly ignored.** `^persistent` was accepted by the splice and demoted to an
  ordinary `let` binding -- i.e. it silently did not do what it said. Both are
  top-level-`def` annotations and now say so.
- **Every `def` annotation is now either accepted or rejected by name.**
  `^linear`, `^relevant`, `^affine`, and `^unique` on a top-level `def` used to
  fall through to a generic arity diagnostic that never mentioned the
  annotation. Each is now refused with its own reason: `^linear` and
  `^relevant` are verified when a binding's scope ends and a global's never
  does; `^affine` would count elaboration sites across the program rather than
  uses at run time, so two functions naming the global would be rejected even
  if only one ever ran; `^unique` asserts no aliasing, which a name every
  function can reach cannot have. An unrecognized `^`-led annotation says
  "unknown annotation" and lists what `def` accepts.

## [0.33.2] -- 2026-08-02

### Added

- **`#writes` write frames -- a checked per-argument write declaration**, behind
  `--enable=write-frames`. `#writes w` / `#writes [a b]` on a `defn` declares
  which arguments the body may write through, and a deferred pass checks the
  declaration against the body: VERIFIED (a fact an optimization may act on),
  EXCEEDED (`TUR-E0382`), or UNVERIFIED (no diagnostic -- the declaration still
  documents intent, and nothing acts on it). The bracket form gives
  `#writes []` -- "writes nothing" -- a spelling, and "no frame" never collapses
  into "empty frame". `#reads` and `#writes` may appear in either order. Gated
  because the checking can reject a body that compiles today.
- **A borrow that provably reaches no writer no longer sinks a refinement
  hypothesis.** WF3 declined a caller body the moment an assigned name was also
  borrowed, because there was no way to ask what a callee does with the borrow.
  With checked frames there is: the decline lifts when every borrow of the
  assigned name is write-free, which is exactly the `frozen`-region idiom whose
  `(& w)` exists only to lock `w` down. "Cannot be written" has three sources
  and no others -- a `^borrow` parameter, a CHECKED `#writes` frame excluding the
  slot, and nothing else; an unresolvable callee or a DECLARED-but-UNCHECKED
  frame both answer "assume it writes".

### Fixed

- **The interpreter copies by-value struct arguments on parameter bind.**
  Turmeric passes structs by value, so a write through a struct parameter is
  invisible to the caller -- the compiled backend has always done this, but turi
  bound the heap `TuriStruct*` straight through, so the same program printed `0`
  compiled and `3` interpreted. The copy recurses through by-value struct fields
  (a nested `(set! (.n (.inner o)) v)` is invisible too) and stops where sharing
  *is* the semantics: an `__rc` wrapper, a `:heap` struct, and any non-struct
  value such as a `&Struct` borrow. stdlib's `Cons` and `Vec` are both `:heap`,
  so list and vector arguments are not walked at all.

### Docs

- **The guides index gets a search filter**, generated by `tools/genguides.py`.
- **WF4 retired** from the checked-write-frames plan -- its premise was false --
  and the stateful-refinements guide updated for the landed WF1/WF2/WF3.
- New reports: the intermittent macOS JIT-leg 45-minute CI hang (with an
  end-to-end confirmation that the `coreutils` timeout contains it), and a
  `definstance` constraint type defaulting to `:int`. New plan: reflected
  measures.

## [0.33.1] -- 2026-08-02

### Changed

- **A refinement hypothesis now survives an assignment that provably cannot
  disturb it.** A crossing's path conditions were dropped wholesale the moment
  *any* `set!`/`swap!`/`reset!` appeared anywhere in the caller body -- the
  coarsest correct rule, and the binding constraint on two shipped surfaces:
  `for-each-alive!` accepts only pure bodies, so an accumulator `set!` about
  `acc` dropped hypotheses about the world and the entity, and every
  `while`-lowered loop lost facts its counter never touched. A hypothesis now
  survives iff every assignment in the body targets a plain symbol the
  hypothesis does not mention, and the body never borrows that symbol. A
  place-expression target (`(set! (.n w) 9)`), an assignment symbol the scan
  cannot attribute, depth or slot exhaustion, or a borrowed target all restore
  the old whole-body decline. The assignment's *value* needs no check: a
  hypothesis is only believed when its terms are congruent, and congruence is
  granted only to a pure measure or to a `#reads` measure inside a region
  freezing its argument -- neither of which a call in value position can stale.

### Fixed

- **`TUR-E0371`'s explainer no longer recommends a retired flag.** It closed
  with "Enable with: `tur build --enable=refined myfile.tur`" -- user-facing
  text pointing at a flag that has been a `TUR-W0063` no-op since refinement
  types graduated in 0.33.0.

### Docs

- **Type-Level Rows (`#row{...}`) added to the HKT guide**, plus a followups
  plan for row types and a report on the stale mono-specs header comment.
- **The refinement solver's shipping status is stated correctly.**
  `advanced-type-system-rationale.md` claimed "there is no SMT dependency. No
  shipped artifact links a solver." The staged decision procedure
  (`refine_solver{_s0,_euf,_arith,_no}.c`) ships compiled into `tur` and runs on
  every compile; what no artifact links is a *third-party* prover -- no libz3 at
  configure time, no subprocess, nothing a user installs. Corrected there and in
  the 0.33.0 CHANGELOG bullet that had inherited the sentence.
- **Refinement plans reconciled with the graduation.** Several documents still
  read as open work or as gated behind `--enable=refined`; status banners,
  prerequisite rows, and struck-through constraints are now dated and accurate.
- A report filed on `pkg_manifest_read` conflating "no manifest" with "broken
  manifest", which degrades a manifest typo into an unrelated `module not found`
  whose hint leads away from the cause -- and on `tur check` printing
  `TUR-E0620` at `error:` severity while exiting 0.

## [0.33.0] -- 2026-08-01

### Fixed

- **A refinement in type-argument position no longer breaks the program.**
  `(Box #refine{ v : int | (> v 0) })` stored the contract node whole and
  nothing peeled it, so the payload a `match` arm binds stayed contract-typed
  and every ordinary use of it failed -- `(+ v 1)` was `TUR-E0006` "first arg
  type { v : int | ... }", `println` found no overload, and a float base was
  `TUR-E0707` "declares float but returns { v : float | ... }" because the
  register-class check compares kinds without peeling. The annotation broke the
  program rather than merely failing to help it. It is now peeled to its base,
  with a new **`TUR-W0380`** stating that the payload predicate is not enforced
  -- inert, but not silently so. Actually checking a container payload needs the
  refinement to survive to the unpacking binder, which is a feature and is not
  built. `TY_CONTRACT` now also delegates to its base in
  `type_has_concrete_codegen_layout`, which this was the last thing blocking.

### Changed

- **Refinement types graduated: `#refine{...}` predicates are now discharged
  statically on every compile.** The `refined` experiment gate is gone. The one
  user-visible consequence: a refinement that is violated on *every* execution
  reaching it is now a compile error (`TUR-E0371`) rather than a runtime
  contract failure. Nothing else changes -- an obligation the solver cannot
  decide still falls back to exactly the runtime check it would have had
  anyway, which is why turning this on cannot make a correct program wrong.
  Graduation covers the stateful slice (`#reads` / `frozen`) as well as the
  pure core. Preconditions were measured rather than assumed: the in-tree blast
  radius was one fixture, and compile cost on a real ~5400-line program was
  1.004x with zero `TUR-E0371`.

  Existing opt-ins keep working and can be deleted at leisure:
  `--enable=refined`, `:experiments [:refined]` and the user experiments file
  are accepted as no-ops with `TUR-W0063`; `#lang turmeric refined` with
  `TUR-W0064`. Both shims age out one minor line from now. `--strict-refine` is
  unaffected and remains a real flag.

### Docs

- **The three refinement guides are reachable by browsing.**
  `refinement-types`, `stateful-refinements`, and `refinement-solver-internals`
  existed but were absent from the guides index -- only `contract-types` was
  listed, so the whole feature was unreachable. They get their own section. The
  advanced-type-system rationale's "refinement types were correctly deferred
  for v1.0.0" section is rewritten as "deferred, then built": the deferral
  reasoning was sound on its premises, but it assumed entailment meant an SMT
  dependency on the compiler's critical path. Because every refinement is a
  contract type first, it already has a runtime meaning, so a partial
  discharger may answer `Unknown` on any obligation and stay sound -- which is
  what made an in-house solver shippable, and why the *external* SMT dependency
  the deferral feared never materialized. Dependent types remain deferred on
  unchanged grounds.

## [0.32.8] -- 2026-08-01

### Fixed

- **`$` in sweet-exp no longer double-applies a rest-of-line that is already
  one complete expression.** `println $ g(7)` expanded to `(println ((g 7)))`
  and failed with "expression in call head has type `int`, which is not
  callable" -- so `$` composed with a bare token sequence but not with a
  neoteric call, a parenthesised form, a curly-infix group or a data literal,
  exactly the spellings the rest of the sweet-exp style encourages. The chained
  form the guides teach, `println $ normalize $ vec3(1.0 0.0 0.0)`, did not
  compile. The wrap is now suppressed when the rest is already exactly one
  balanced, delimited expression. A bare atom is deliberately untouched:
  `f $ g` stays `(f (g))`, SRFI-110's zero-argument reading.

- **A capturing closure passed to a tyvar-signature fn parameter no longer
  segfaults.** `(defn run [R] [body : (fn [] R)] : R (body))` compiled clean
  and then jumped into the closure's env struct, because a tyvar-signature
  parameter kept the thin representation, which has nowhere to put an
  environment. Fat normalization now admits tyvar signatures on the *parameter*
  side; the result side deliberately keeps the concrete-only claim, since the
  poly-call protocol boxes returned closures itself. Two missing shim sites came
  with it: rank-2 forall params called through the carrier, and normalized
  params stored into fat struct fields, which were boxed a second time. An
  effect-row signature is still excluded and tracked as a fuzz `--known-probes`
  row.

- **A `{ shim, orig }` fat box no longer leaks once per call at a normalized fn
  parameter.** Nothing frees a box handed to a normalized nominal param, so
  `(apply1 add3 acc)` in a loop leaked 24 bytes an iteration -- 5e6 iterations
  peaked at 122 MiB. Where the boxed value is a file-scope function the contents
  are constant, so the box is allocated once at file scope, filled from
  `__tur_static_init`, and given a no-op drop glue so every drop path correctly
  does nothing. Peak RSS 122 MiB -> 1 MiB; a 2e7-iteration loop 0.533s ->
  0.042s. The hoist is opt-in per shim site and only the normalized-nominal-param
  site takes it -- `^fat` sinks and owning struct fn-fields keep the heap box,
  since a `^fat` callee may drop its argument. The same per-call box at a `^fat`
  sink leaks too and is filed as its own report.

- **Generated C no longer straddles the int64 carrier and a pointer at a
  monomorphized constructor's field slot or at a fn-value return site.** Four
  programs (`conv-defstruct-option-fn-element`, `hkt-ap-fn-in-container`,
  `defalias-composite`, `fn-value-matrix-ok-rows`) failed to compile on any
  toolchain that promotes `-Wint-conversion` to an error -- Apple clang >= 15
  and gcc >= 14 -- while the older gcc on the CI Linux leg merely warned. This
  was the sole remaining cause of the standing red `Test (macos-latest)` job.
  Both halves come down to not re-deriving an emitted C type from a `Type`: the
  constructor's real parameter C type is now recorded when the ADT application
  is registered and looked up at the call site, and the return-site bridge asks
  the typed AST whether the tail expression emits the carrier rather than
  pattern-matching the emitted string. No codegen snapshot moved.

- **A `(c-fn ...)` and an ordinary `(fn ...)` of the same signature no longer
  collide on one monomorph C name.** The type checker already holds the two
  distinct whenever the latter is a capturing closure -- a fat closure must
  never flow into a raw C callback sink -- but the type mangle did not carry
  the `cfnptr` flag, so `(Option (c-fn [int] int))` and `(Option (fn [int] int))`
  produced two registry entries under one name. The second `#ifndef` block was
  preprocessed away and the `c-fn` view silently adopted the closure view's
  `void *` constructor slot in place of its own function-pointer typedef, with
  **no diagnostic from any compiler**. Benign in practice only because every
  function-value representation is a same-bits 8-byte word.

  Splitting the names then exposed a latent ordering bug the collision had been
  hiding: function-pointer typedefs are now written before the monomorphized ADT
  definitions that reference them (they are still *generated* after, since
  emitting a monomorph is what registers them). `type_eq` is deliberately
  unchanged.

- **A generic whose result family is instantiated at a parametric ADT no longer
  calls the wrong monomorph.** Both specializations of `vec-empty-like__` called
  the `int`-element `vec-new` clone; the `(Map sym int)`-element one was never
  interned or emitted. A zero-argument, return-only-polymorphic call records no
  type-variable bindings at elaboration, so its callee monomorph is recovered
  from the enclosing specialization's result family -- and that recovery gated
  each element on `type_has_concrete_codegen_layout`, which returns false for
  every type application by design (its own comment names
  `type_app_is_concrete_adt` as the companion predicate for a concrete
  parametric ADT). Consulting only the first meant every ADT-application element
  was silently declined; the `int` clone worked only because `int` is not a type
  application. Runtime-benign where it was found, purely because `vec-new`'s body
  is element-agnostic.

### Internal

- **Six GC / `Rc` / weak-reference fixtures assert on the CG6 collector counter
  (`gc-live-blocks`) instead of a process-wide malloc probe.** The probe equals
  the program's heap only on an unsanitized `cc` build that owns its process; it
  was already known to be vacuous under ASan on glibc and quarantine-inflated
  under ASan on Darwin, and under one-process `tur jit` it reads the compiler's
  heap. That last mode was the whole of the `JIT engine (macos-latest)` redness
  -- never a GC, `Rc`, weak-reference, or JIT-codegen defect.

  The counter is program-scoped, exact rather than tolerance-based, portable,
  and identical across every linkage mode, so the six now run under the JIT and
  under `cc` with the same output. The assertions got stronger: the
  collector-off control moved from *impossible* (identical output either way on
  glibc) to `10000` against a tolerance of `0`. Five of them also stopped
  falling back to `cc` under the JIT engine, since the probe's
  `#include <malloc/malloc.h>` was what dragged in the `TargetConditionals.h`
  the MIR front end rejects.

  `tests/run-gc-leak-gate.sh` gained four real assertions (14 passed / 2 skipped
  -> 18 / 2) because `gc-collects-strong-cycle` could move from its
  probe-output exemption list to the controls. The byte-level probe survives as
  `gc-collects-strong-cycle-heap-bytes`, `cc`-only, since bytes still catch a
  payload leak the block counter cannot see.

- **Interpreted trampoline fixtures cap their RSS, and a per-fixture timeout is
  reported as a timeout rather than a stdout mismatch.** The tree-walking
  interpreter retains roughly 4 KiB per trampolined step, so a fixture's step
  count is a memory multiplier under `--interpret` and two co-scheduled large
  ones were memory pressure, not a CPS defect. Diffing stdout first had been
  turning a killed fixture's partial output into a claim about the answer.

- **CI**: the Windows leg installs `wineditline` and compares the smoke fixture
  in bash (`diff` is not present on the runner); the macOS JIT baseline is
  re-measured in the job's own configuration, and the JIT corpus counts are
  published on a passing run.

## [0.32.7] -- 2026-08-01

### Added

- **A `windows-latest` CI job.** The Windows build is covered on every push
  rather than rediscovered by hand, so a break in that path surfaces in CI
  instead of at release time.
- **`tur experiments` and `tur lang-layers` appear in `tur --help`**, alongside
  the `--enable=<name>` global flag. All three shipped without a listing.

### Fixed

- **The Windows build works again.** The emitter, LSP, arena, REPL, and the
  generated runtime-split sources build on Windows, and `platform_fs.h` grows
  the shims that path needed.
- **Winsock socket options.** `setsockopt`/`getsockopt` are shimmed in the
  Winsock compatibility layer, and `SO_REUSEADDR` -- whose Windows semantics
  are not the POSIX ones -- is no longer set there.
- **`stdlib/fs` and `stdlib/term` on Windows.** Their inline-C bodies are
  ported off POSIX-only APIs; fixtures that genuinely require POSIX I/O now
  carry a `requires.posix-apis` marker and skip rather than fail.
- **Nested `bind` over `result` no longer segfaults at a typed boundary.**
  The emitter now agrees with itself about the carrier across the boundary.
- **turi resolves return-directed class methods from the frame's pinned type
  variables**, instead of keeping an instance baked in from an earlier call.

### Internal

- `tests/run-jit.sh` times negative fixtures through `_run_timed` rather than a
  bare `timeout`, cutting the JIT harness's false failures from 407 to 6.

## [0.32.6] -- 2026-07-31

### Added

- **`tur jit <file>` -- an in-process MIR JIT engine.** Behind two gates
  (`-DTUR_JIT=ON` at build time, `--enable=jit` at run time), `tur jit`
  compiles a program's emitted C with c2mir and runs it in process -- no `cc`
  subprocess, no linker. Any engine failure prints `TUR-W0070` and delegates
  to the existing `cc` path, so the subcommand never fails where `tur run`
  would have succeeded. A default build vendors nothing and carries no new
  dependency.
- **A split runtime: the JIT compiles against declarations, not the preamble.**
  `tur jit` swaps the emitted all-gates preamble for a committed declarations
  region under an xxh64 hash guard and resolves the runtime by address into
  the host; on a mismatch (emitter drift, knob drift, missing archive) the
  full preamble is used unchanged. `arith` end to end drops ~278ms -> ~200ms.
  `TUR_JIT_NO_SPLIT=1` opts out.
- **The REPL builds spices in process.** `tur --enable=jit repl` replaces the
  `tur build --shared` subprocess + `dlopen` + `dlsym` pipeline with the
  engine; cold spice load drops ~850ms -> ~260ms on a two-module probe.
- **The JIT engine is reachable from a libturi embedder.** A C host linking
  `libturi` probes for `TUR_HAVE_JIT` (a PUBLIC compile definition, so the
  probe resolves whether or not an engine was built) and drives the engine
  through `jit_engine.h`, falling back to `turi_eval` rather than to `cc`.

### Fixed

- **Emitted C is portable to a strict C11 front end.** `__auto_type`,
  `__attribute__((constructor))`, `__attribute__((cleanup))`, and
  `__extension__ ({...})` no longer appear in the emitted program -- replaced
  by named types, an explicit `__tur_static_init()`, an explicit scope-exit
  pop, and a bare statement expression. This is what unblocked the JIT, but it
  makes the `cc` path's output more portable too.
- **Invalid C from two emitter defects.** A call temp now records its
  *declared* C type rather than a re-derived one, and a `TUR_APPLY` argument
  is no longer cast to an aggregate parameter type.
- **`return <void expr>;` is no longer emitted in `:void` functions**, and
  hoisted `#include`s are ordered before hoisted code.
- **Persistent-map `cstr` keys hash and compare by content**, not by pointer
  identity, in the P3 `^persistent` lowering.
- **Dynamic bindings pop on an early return**, not only via the cleanup path.
- **Self-TCO fires through a capturing named `let`.**
- Two arm64/macOS emitter defects: the `xxh64` prototype and the
  `TaskGroupBlock` layout.
- Monaco editor colors in the web REPL.

### Internal

- CI covers the MIR JIT engine (`tests/run-jit.sh`), and a parity harness plus
  an engine benchmark triangle compare `tur jit` against the `cc` path across
  the fixture corpus.

## [0.32.5] -- 2026-07-30

### Removed

- **The Z3 refinement oracle scaffold is retired.** The dev-only Z3 backend
  (`refine_libz3.c`), the `TUR_REFINE_Z3_ORACLE` CMake option, its
  `find_package(Z3)` block, and the `tur_refine_fuzz` VC-level differential
  fuzzer are deleted, having met both retirement criteria. No shipped artifact
  ever linked Z3 -- the option defaulted off and refused Release and WASM
  builds -- so the compiler is functionally unchanged. Solver soundness is now
  guarded by the labelled SMT-LIB corpus (`tur_refine_corpus`: 125 benchmarks,
  no solver linked, 0 soundness failures) and the source-level fuzzer
  `tests/refine-fuzz-src.py`, which never needed an oracle. The internal
  diagnostic `TUR-I0379` is retired with it; its code stays reserved.

### Docs

- **Automatic GC is permanently opt-in.** The `cycle-gc` plan's CG8 phase no
  longer conflates ungating `(gc-auto!)` with making `GC_AUTO` the default;
  only the ungate is on the table, and the default-on option is recorded as
  rejected rather than deferred.
- `expires_at` is documented consistently as advisory -- it never blocks a
  release cut.

## [0.32.4] -- 2026-07-30

### Fixed

- **Function-typed values now carry a single ABI shape.** Fn-typed values are
  fat-normalized at return, ascription, and concrete-signature nominal
  parameter positions, and carrier/fat provenance is tracked through aliases
  and type joins. This closes a family of miscompiles where a closure passed
  or returned across a typed boundary produced invalid C or a SIGBUS.
- **`bind` continuations pair with the selected entry point.** Monadic `bind`
  at a typed boundary (e.g. `Result`) no longer emits a continuation whose ABI
  disagrees with the callee it was selected for.
- **Class-method results into generic positions.** `__inst_` callees with
  by-value results are no longer treated as carrier producers, fixing invalid
  C from typeclass method results flowing into generic contexts.
- **Width-independent container elements.** By-value struct elements in `vec`
  and narrow struct values in `map` round-trip correctly under both `tur` and
  `turi` instead of depending on the element's machine width.
- **Heap-record bindings consult the representation spec.** Concrete heap
  bindings (including wide lens families) get correctly typed pointer
  bindings, and transparent int newtypes are treated as their payload.

### Internal

- `repr_of(type, position)` is now the single decision function for value
  representation, backed by a one-row-per-`TypeKind` table, shadow
  instrumentation, and a representation-decision ratchet in the test suite.

## [0.32.3] -- 2026-07-30

### Added

- **Numeric tower: `Rational` and `Complex`** (`stdlib/rational.tur`,
  `stdlib/complex.tur`). `Rational` is an exact `num`/`den` pair over int64,
  always normalized, so structural equality is mathematical equality;
  `Complex` is a plain `re`/`im` pair of doubles. Both are pure Turmeric with
  no inline C, so they behave identically under `tur` and `turi`. Complex
  division uses Smith's algorithm, and the emitted C never contains
  `_Complex`, `<complex.h>`, or the `__mul*c3`/`__div*c3` compiler-runtime
  helpers. Complex deliberately has **no** `Ord` instance. See
  [docs/guides/numeric-tower-guide.md](docs/guides/numeric-tower-guide.md).
- **Operator overloading via `Num`**: when no builtin operator row matches the
  argument types, `+`/`-`/`*`/`/` now fall back to `Num` typeclass dispatch --
  so any user numeric type with a `Num` instance works with the bare
  operators. Variadic calls left-fold into nested binary method calls, and
  unary `-` reaches `neg`. Primitive arithmetic is untouched: the builtin row
  still wins whenever it matches.
- **`#rat{3/4}` and `#cx{3.25 -1.5}` reader literals**, alongside `#map{...}`
  and `#set{...}`. `#rat{...}` reads its body raw (curly-infix would otherwise
  make `{3 / 4}` infix division) and normalizes at read time, so `#rat{6/8}`
  and `#rat{3/4}` are the same literal; a zero denominator is a read-time
  error (`TUR-E0284`). `#cx{...}` reads two ordinary expression slots, so
  `#cx{3.25 {1.0 + 0.5}}` composes (`TUR-E0285` on any other arity).
- **`exp`, `log`, `sin`, `cos`, `atan2` in `stdlib/math.tur`**, with matching
  interpreter natives; `fabs`, `ceil`, and `pow` gained the interpreter
  natives they were missing.
- **`Num [float]` instance**, which the class was missing.
- **Editor intelligence in the browser**: the LSP gained a WASM bridge and a
  browser analysis backend, so Try Turmeric now offers completion, hover,
  diagnostics, and go-to-definition without a server. Transport is split from
  dispatch, with transport-free session unit tests behind it.
- **`defalias` accepts a full type expression** -- not just a type constructor
  -- so composite aliases (function types, rows, applications) are
  expressible.
- **`defopaque :sealed`**: `::` cannot cross the representation boundary of a
  sealed opaque type.
- **`:tur-version` in `build.tur`**: a spice can declare which compiler
  versions it is known to work with.
- **Shadowing diagnostic**: a `defn`/`defmacro` that shadows a special form
  now warns at the definition site rather than surprising you at the call.

### Changed

- **`Num` class methods are closed over the instance type**: `add`/`sub`/
  `mul`/`div`/`neg` now take and return `a` instead of returning `:int`. The
  existing instances needed no body changes, but emitted dictionaries and
  instance methods now carry the instance's own type (e.g.
  `__inst_Num_add_int8` returns `int8_t`, not `int64_t`). This is what makes a
  `Num` instance for a non-integer type -- Rational, Complex, a user newtype --
  usable at a call site.

### Fixed

- **Higher-kinded dispatch inside constrained-polymorphic bodies** now goes
  through dictionary passing ("Route B"). This fixes return-directed method
  resolution on a constrained abstract type constructor, aggregate
  continuation returns at the dict-dispatch carrier, and the
  partial-application hole in the `Type`.
- **ICE when taking an effectful function's address** -- it was counted as
  performing the effect.
- **SIGSEGV calling a `^fat` parameter with a non-empty effect row.**
- **Name mangling**: `append_type_mangle` is now injective (its default arm is
  gone), and C reserved words are guarded with a `tur_u_` prefix.
- **Codegen**: a stored `fn` element is spelled as a handle with its type
  variables substituted, and `:Sym` is treated as a concrete codegen layout.
- **Row algebra threads field names through**, so typed-field rows survive row
  operations.
- **`load` honors a loaded file's inline `#lang`**, and `--interpret` now
  routes through the same path.
- **The transitive `:spices` include-path walk terminates on cycles.**
- **`turi` shows collection elements by their own `Show` instance** rather
  than always using `Show[int]`.
- **`build.tur` bare-brace manifest syntax** in the docs and the manifest hint.

### Docs

- New guides for type-level rows, value representations, and the numeric
  tower; `effects-vs-monads` rewritten around the present-tense choice.

## [0.32.2] -- 2026-07-27

### Added

- **LSP: formatting, signatureHelp, and `$/cancelRequest`**: `tur lsp` now
  answers `textDocument/formatting`, `textDocument/signatureHelp`, and
  `$/cancelRequest` -- the largest protocol gaps found by building a real
  editor client (Trowel) against the server.
- **REPL: `:load-string` and richer shell-integration markers**:
  `:load-string "<src>"` evaluates a literal without a disk round-trip; OSC
  133 `C`/`D` now bracket evaluation so a host can distinguish idle/busy/done,
  and `TUR_SHELL_INTEGRATION=1` forces markers on when driving the REPL over
  a pipe.

### Fixed

- **LSP completion no longer goes blank while typing**: the symbol index is
  retained across a failing compile and primed even for a file that has
  never parsed, so an unbalanced paren -- the normal state mid-edit -- no
  longer drops completion to zero.
- **LSP hover, diagnostics, and sync papercuts**: hover honors
  `hover.contentFormat`, zero-width diagnostic ranges are widened
  server-side, `didChange` reads the last (not first) content-change entry,
  and the analysis temp file no longer hardcodes `/tmp` (which broke on
  Windows).
- **`\uXXXX` JSON escapes decoded correctly** in LSP messages, including
  surrogate pairs -- previously the backslash was dropped, corrupting text
  and shifting every later diagnostic's byte offset.
- **REPL working-directory reporting uses `pwd -L` semantics**: a
  symlink-resolved `getcwd()` no longer disagrees with a host's own path
  tracking (e.g. `/tmp/demo` vs `/private/tmp/demo` on macOS).
- **`tur fmt` no longer mangles a `defn` with a type-parameter vector**;
  stdlib reformatted to match.
- **`build.tur` manifests with map values no longer error** during parsing.

### Changed

- **LSP analysis is debounced and negotiates `positionEncoding: utf-8`**,
  cutting redundant compiles on rapid edits and making the byte-offset
  contract explicit instead of a silent mismatch with the LSP default.

## [0.32.1] -- 2026-07-27

### Added

- **REPL `:cd` and `:pwd`**: `:pwd` prints the working directory and `:cd
  [dir]` changes it (bare `:cd` goes to `$HOME`). This moves the *running*
  process, so definitions and session state survive -- unlike a host-side
  "set directory", which can only restart the REPL. A successful `:cd` also
  emits an OSC 7 `file://` report when shell integration is active, alongside
  the existing OSC 133 prompt markers, so a host editor can track the working
  directory without restarting or scraping output; the initial directory is
  reported at startup for the same reason.
- **`stdlib/rcvec`**: a flat vector of `rc<A>` that the cycle collector can
  trace. A plain `Vec[rc<T>]` is refcount-correct but invisible to the
  collector, so a cycle through a slot strands; an `RcVec` carries its own
  walk/drop hooks, so cycles through it are reclaimed.

### Changed

- **Long-lived interpreter sessions bound their memory**: tracked collection
  buffers (Vec/Set/Map wrappers, TVar cells) are swept at the eval boundary
  right after a successful scratch-promotion rewind, instead of accumulating
  until teardown. Measured over 5000 transient-vec evals: 5000 tracked boxes
  retained before, 0 live after.

### Fixed

- **`rc` scalar default drop glue no longer frees its inline payload**: a
  decrement-to-zero on a scalar `rc` allocated with default glue freed an
  interior pointer, aborting under ASan. Scalars now default to a no-op
  inline drop; the separate-payload entry points keep the freeing default.
- **TVar cells survive promotion rewinds**: cells were scratch allocations
  that promotion could not see, so the first rewind after
  `(def t (tvar/new 0))` poisoned the cell -- a live use-after-reset in the
  REPL. Cells are now tracked boxes that survive rewinds and sweep when
  unreachable.

## [0.32.0] -- 2026-07-26

### Added

- **`#reads` stateful refinements (experimental)**: extend `--enable=refined`
  with a stateful surface -- `#reads` annotations on parameters (parsed,
  congruence-granted at call sites, and backed by codegen), the frozen-region
  form realized as a linear mutation cap, and boolean-sorted measures in
  refinement predicates. Macro templates can emit `#reads` annotations and
  `#refine` contract types.
- **`tur test` directives**: per-test flags and expected-error negative
  tests, so refined/negative fixtures run under plain `tur test`.
- **Arena debug diagnostics**: debug-poisoning and guard diagnostics that
  surface uninitialized-arena reads early.

### Changed

- **Inline-C HKT instances return real results**: the by-value HKT result
  limitation is lifted -- an inline-C `definstance` body can return a
  carrier-width or `:heap` ADT result without heap-boxing, `stdlib/rc.tur`
  compiles again, and TUR-W0042 diagnoses the remaining unsupported shape
  at the `definstance`.

### Fixed

- **Refinement guard discharge**: crossing guards are now collected from the
  whole function body (not just the return form) and from inside macro
  expansions, so macro-generated guards and frozen-region crossings
  discharge; unproven `#reads` crossings surface in non-strict mode; the
  refine memo resets per compile (fixes multi-compile corruption).
- **defstruct/ADT field elaboration**: fields resolve assoc-type projections
  and Size literals, fixing the `(Storage T)` compiler skew.
- **Arena crashes**: two uninitialized-arena-read crashes.
- **Stdlib load dedup**: `(load "stdlib/X")` resolves via the stdlib dir
  first, so it no longer double-loads against the auto-loaded stdlib.
- **Weak refs**: execute the stdlib weak-ref audit (WR1, WR3, WR4).

## [0.31.1] -- 2026-07-26

### Added

- **`Show [Sym]`**: add a Show instance for runtime symbols, and close
  deeper sym-show display gaps.

### Changed

- **Cycle-GC pause bounds**: bound linearization, add measured caps,
  memoize macro expansion, and make the `rc<T>` free-queue drain linear
  (no per-link recursion) so large cycles collect without a stack overflow.

### Fixed

- **`set!` rc<T> leak**: `set!` now releases the `rc<T>` value it
  overwrites and normalizes what it stores.
- **Cycle-GC pause time**: fix the collector's real pause-time term (a
  quadratic over the candidate set, not the candidate set itself).
- **Refinement types**: cross the caller's whole body at a call site,
  sort measures by return type, type SMT-LIB corpus numerals by the
  declared logic, and round-trip refined syntax correctly through `fmt`.
- **Linearity**: a closure that captures and consumes a linear value is
  now itself linear.
- **`#lang` reader switch**: keep the preloaded stdlib across a reader
  switch.
- **wasm32**: mix `promo_hash` at a fixed 64 bits so wasm32 no longer
  shifts past the hash width.

## [0.31.0] -- 2026-07-25

### Added

- **Refinement types (experimental)**: predicate-refined types behind
  `--enable=refined` (or `#lang turmeric refined`). Ships RT0-RT7 -- refined
  parameters/results, call-site and let/match path-condition crossings,
  typeclass method result-refinement enforcement (TUR-E0374/E0375), and a
  three-valued purity classifier. Backed by an in-house SMT solver plus an
  optional Z3/cvc5 oracle, a source-level fuzzer, and a labelled SMT-LIB
  corpus replayed against the chain. Runs on wasm32.
- **Cycle-collecting GC (experimental)**: a Bacon-Rajan trial-deletion
  collector behind `--enable=cycle-gc` reclaims strong `rc<T>` reference
  cycles via `(gc!)` / `(gc-auto!)`.
- **`rc<T>` in collections**: `Vec`, `Map`, and `HAMT` can own `rc<T>`
  elements, with caller-supplied value ownership; adds `stdlib/rcchain.tur`,
  a collection the cycle collector can trace.

### Changed

- **turi REPL incremental elaboration**: persistent elaboration/parsing
  session takes long-lived envs from O(N^2) to ~O(N); the incremental path is
  now ON by default.
- **Single runtime GC**: compiled executables link the runtime collector
  instead of replicating it per module, reconciling the two `RcControlBlock`
  layouts and guarding against future drift.

### Fixed

- **Refinement codegen defects**: fix two codegen bugs in `match` on a
  non-ADT scrutinee, `rc/of` over a multi-variant ADT (released/traced
  nothing), `rc<T>` over a `:heap` defstruct, and a compiler stack overflow
  on a disjunctive refinement goal.
- **Web REPL**: invoke top-level `main` so Run shows output (not
  `#<fn main>`), unstick the service-worker cache so the REPL loads the
  current wasm, and add a "Force update" command.

## [0.30.8] -- 2026-07-24

### Fixed

- **Interpreter (`--interpret`) parity**: retain `rc<T>` on closure capture,
  resume multishot/cross-fn continuations correctly, and close String-parity
  fixture gaps so the tree-walking interpreter matches compiled behavior.
- **Async catch-unwind**: keep `catch-unwind` stackless inside async programs
  (fiber-rec).
- **Nested effect handlers**: resolve non-termination when handlers are nested
  (effect-rec).
- **macOS build & codegen**: broad macOS compatibility fixes across the
  compiler, runtime, and stdlib, plus CPS cons/cstr argument casting and
  by-value aggregate frame-escape codegen fixes.

## [0.30.7] -- 2026-07-23

### Added

- **Curated `#lang` layers**: `#lang <base>[/<dialect>] <layer>*` selects one
  mutually-exclusive base reader (`turmeric`, `turmeric/curly-infix`,
  `turmeric/neoteric`, `turmeric/sweet`) plus an order-independent set of
  additive layers. Ships the `stringed` reader layer (`#s"..."`) and validates
  every layer token against the curated `LANG_LAYERS[]` table -- an unknown
  layer is a hard error, not a silent ignore.

### Docs

- Reader-forms and syntax guides document the base/layer split; the web REPL
  handles the new `#lang` line form.

## [0.30.6] -- 2026-07-23

### Fixed

- **Composed lens codegen**: lower composed struct-of-closures lenses end to
  end -- specialize every closure of a struct-of-closures return and mangle
  apostrophes in ADT monomorph names (Blocker 2b/2c).
- **Effectful fn-value params**: thread effectful callbacks correctly through
  fat-closure function-value parameters (E2).
- **catch-unwind leaks**: reclaim the caught result box and panic-message
  string on unwind.
- **`#lang` line parsing**: strip trailing tokens on the `#lang` line before
  handing source to the reader.
- **Separate compilation (`--shared`)**: unblock `--shared` spice builds, order
  base ADT typedefs ahead of the monomorph flush in the header, and mirror the
  direct forward-decl param ABI in the CPS entry-wrapper.
- **libc symbol collisions**: mangle user globals whose names collide with libc
  symbols.
- **Imported-module defns**: forward-declare load-spliced top-level defns in
  imported modules.
- **stdlib/httpd leaks**: free per-limiter `RateLimit` state at process exit and
  per-request cookie/form accessor strings.
- **stdlib/image**: hoist platform executable-path includes to file scope.
- **REPL builtins**: inject native-function stubs so `cons`/`head`/`tail`
  resolve at the prompt.

## [0.30.5] -- 2026-07-22

### Fixed

- **REPL `:reset` / `:run` restore the full stdlib surface**: recreating the
  session env (via `:reset`, or an editor "Run" that resets first) no longer
  drops the interactive stdlib preload. `list-head`/`list-tail` and other
  carrier helpers stop emitting spurious `TUR-W0040` warnings, and `#map{}` /
  `#set{}` and typeclass `Show` work again after a reset instead of failing
  with "unknown function or operator".

## [0.30.4] -- 2026-07-22

### Added

- **`tur compile` / `tur link` subcommands**: `tur build`'s compile and link
  phases are now separately invokable, and `tur build --runtime=lib` links a
  prebuilt `libturi.a` instead of recompiling the runtime. A lean, non-ASan
  `libturt_runtime.a` ships so `--runtime=lib` is defaultable.

### Changed

- **Automatic closure/Drop reclamation graduated**: the drop-glue header ABI is
  now the default (closure-drop-glue R4). The compiler emits scope-exit
  auto-drop for move-only `Drop`-instance opaque let-bindings, letting httpd
  retire its manual `httpd-mw-drop` / `httpd-mw-free-chain` markers and reclaim
  runtime-built middleware chains automatically.
- **Default runtime linkage is now auto**, preferring the lean prebuilt archive.
- **Interpreter FFI arity ceiling lifted** via per-export shims.

### Fixed

- **stdlib `String` load in imported modules**: fixed via a forward-decl
  pre-pass (#705).
- **Leak of module-private env keys** in the interpreter (LSan-reported) (#704).
- **Flag-on crash clusters** cleared during graduation prep (33 -> 3), plus
  `^fat` handler drops on httpd construction-failure paths.

## [0.30.3] -- 2026-07-21

### Added

- **Owned `String` builders and optional accessors**: `stdlib/str-build-string.tur`
  adds `str-concat-string` / `cstr-sub-string` (owned-`String` wrappers over the
  `cstr` builders), with wrap-vs-build guidance steering multi-join accumulation
  to `StringBuilder` (linear) rather than an O(n^2) fold. `stdlib/httpd-string.tur`
  adds `httpd-req-cookie-opt` / `httpd-req-form-opt` returning `option<String>`, so
  "present but empty" (`some ""`) is distinct from "absent" (`none`) (#701).
- **Owned `String` sibling modules for the stdlib**: opt-in `*-string` modules
  wrapping each freshly-allocated-`cstr` function in `string/adopt-cstr` --
  `json-string`, `csv-string`, `term-string`, `re-string`, `range-string`, and
  `schema-string` (Bucket A) (#702).

### Changed

- **Leak-clean fixtures**: dropped 6 stale `requires.no-leak-check` markers now
  that the S1/S2 fat-closure drop machinery reclaims the escaping / HOF-passed
  value-closure envs the opt-outs were guarding (#703).

### Fixed

- **Macros invisible across stdlib re-elaboration**: fixed macros not resolving
  across stdlib re-elaboration (`elab_core.c`, `eval.c`, `fiber.h`).

## [0.30.2] -- 2026-07-21

### Fixed

- **`void*`/`int64_t` carrier straddles for `String` returns**: compiled
  `String`-returning functions (and taskgroup handles) emitted C that straddled
  `void*` and `int64_t`, which clang's default `-Wint-conversion` and GCC 14+
  `-Werror` reject as a hard error -- blocking AOT-compiled use of the owned
  `String` type and reddening the macOS CI leg. The `Show [..] : String`
  inline-C bodies, the `emit_expr`/`emit_fns` return paths, and the
  phantom-witness carrier now bridge through `(int64_t)(intptr_t)` /
  `(void*)(intptr_t)` (#699). This also fixes interactive `show` of results in
  `tur repl`: expressions such as `(+ 40 2)` previously displayed a corrupted
  value (e.g. `#<fn main>`) instead of `42`.
- **Additional macOS codegen fixes**: further carrier-straddle and CPS-IR emit
  fixes surfaced by the macOS toolchain (`emit_expr.c`, `emit_cps_ir.c`,
  `main.c`).

## [0.30.1] -- 2026-07-21

### Fixed

- **Generic `show`-wrapper monomorphization + `rc`-field double-free**: fixed a
  monomorphization bug in the generic show-wrapper and a double-free of
  refcounted struct fields (#697).
- **CPS effect-loop leak**: plugged an O(N) DK-node leak on tail-resumed effect
  re-opening (#696).
- **`Real-Random`/`Seeded-Random` codegen**: fixed nested static-fn C emit for
  the random instances (#693).

### Changed

- **CPS `shift`/`reset`**: serial and cloneable continuations are now folded
  into `shift`/`reset` by receiver-continuation capability (#695).

### Added

- **Trowel landing page**: a new web landing page for the Trowel toolchain.

### Docs

- **Strings guide**: added `docs/guides/strings-guide.md`, cross-referencing
  `cstr` vs owned `String` ownership (#698).

## [0.30.0] -- 2026-07-20

### Added

- **Owned String type**: a new owned, immutable, refcounted `String` type
  (owned-string-type-plan), with an opt-in `#s"..."` owned-String literal and
  a stdlib reader-macro path.
- **StringSlice**: zero-copy, bounds-checked, safe ranged views into a `String`.
- **`ShowString` typeclass**: an owned-String show surface, plus a
  `derive-show-string` derive macro, `ptr<void>`/`Bound` instances, and
  `Debug`/`Display` instances for `cstr`, `bool`, and the numeric types.

### Changed

- **`Show` returns an owned String**: the `Show` typeclass now returns an owned
  `String` rather than a `cstr`. `derive-show` is now the owned-String deriver;
  the old `cstr` path moves to `derive-show-cstr`. Interpreter Show is at parity.
- **stdlib String adoption**: stdlib migrates onto the owned `String` (owned
  bridge + path cluster, digest, and httpd CORS capture).

### Fixed

- **`rc<int>` angle-bracket annotation**: no longer silently becomes a type
  variable (#692).
- **Generic typeclass dispatch on opaque carriers**: fixed, along with derive
  alias labels.

## [0.29.1] -- 2026-07-19

### Fixed

- **gcc14 clean build**: a tree-wide representation-tracking sweep now casts
  int64<->pointer carriers correctly throughout codegen (spec-dispatch,
  cps->direct tail calls, ctor field args, inline-C anon-structs, control-form
  result assignments), so the compiler builds clean under gcc14 -- the
  `-Wno-error=int-conversion` and `-Wno-error=incompatible-pointer-types`
  escape hatches have been dropped.
- **effectful-fnvalue miscompile**: fixed a miscompilation of a polymorphic
  function value passed through the CPS ABI.
- **cps->direct dispatch**: cps->direct LETCALL now resolves to the defined
  monomorph clone, fixing an unmangled tcons reference.

### Changed

- **CPS owning-env teardown graduated (E3a/E3b)**: capturing an owning value
  into a multi-shot cloneable continuation is now always-on, including deep-copy
  clone of captured heap handles and auto-deferred scope-exit drops across a
  cloneable reset.
- **cps-async graduated**: the remaining fiber-interop gaps are closed and async
  CPS is now always-on.
- **Relaxed cloneable-shift requirements (E4a)**: an owning or non-Serializable
  value merely in scope at a cloneable shift no longer requires `Clone` /
  `Serializable`.

## [0.29.0] -- 2026-07-18

### Removed

- **The fiber-based effect runtime is deleted**: the legacy fiber effect
  handler runtime -- the second effect-lowering substrate that ran
  alongside the CPS/DK backend -- has been fully removed (Stage G). The
  delimited-continuation (DK) backend is now the sole lowering path for
  effects and handlers, and the dead `tur_effect_cont_resume` /
  `emit_effects_*` machinery is gone (#685).

### Changed

- **The CPS/DK backend is the sole effect-lowering path**: effectful
  handles (top-level, macro-expanded, and inside `defmodule`), first-class
  and dynamic handler values, effect-polymorphic higher-order functions,
  nested handles, escaping continuations, and self-handling async closures
  now all lower onto the DK backend instead of evicting to a fiber.
- **`cps-tramp-resume` graduated to always-on**: trampolined tail-resume
  is no longer experimental, giving flat (heap-bounded) effectful
  tail-recursion at scale (e.g. 1e6 iterations) with no flag required.

### Added

- **Windows bringup**: the compiler and runtime now build and run on
  Windows (#682).
- **`stdlib/logic.tur` is pure Turmeric**: the miniKanren logic engine was
  reimplemented with no inline C (#679).
- **`stdlib/re.tur` is a pure-Turmeric regex engine** (#676).
- **Cooperative session-channel runtime** under the interpreter (#677).
- **Shallow effect handlers (F2) and async/await on heap continuations
  (F3)** (#674).
- **Unbounded function arity**: the hard positional-parameter cap is
  removed; wide functions store args out-of-line, with a `TUR-W0041` lint
  nudge past 16 params (#669).

### Fixed

- **Numerous effect-runtime memory leaks closed**: DK continuation-chain
  nodes, straight-line and heap-join perform-continuation frames, shift
  receiver closure environments, and multi-shot resume snapshots are now
  freed; the compiler/codegen path is leak-clean under ASan/LSan
  (#681, #664, and follow-ups).
- **`Eq [Bound]` no longer misdispatches to bare-tyvar instances**; adds a
  `str-build` leaf (#673).
- **Owning `rc` capture into a multi-shot handler-case environment** is
  handled correctly (#680).

## [0.28.2] -- 2026-07-12

### Fixed

- **CPS backend temporaries are now `__`-prefixed to avoid user-code
  collisions**: the CPS-IR-to-C backend previously emitted temporary
  variables under bare names that could collide with identifiers from user
  code; every generated temporary is now `__`-prefixed, closing that
  namespace hazard (#661).

## [0.28.1] -- 2026-07-11

### Added

- **Debug sanitizers are now configurable via `TUR_DEBUG_SANITIZE`**: the
  ~40 hardcoded `-fsanitize=address,undefined` sites in the Debug build now
  route through a single `TUR_ASAN_FLAGS` variable driven by a new
  `-DTUR_DEBUG_SANITIZE=ON/OFF` CMake option (also gating the `eval_import`
  test's fixture-compile flags), so ASan/UBSan coverage can be toggled without
  editing the build. Adds a tight-timeout `tur --version` CI smoke check on
  Linux and macOS to catch startup-hang regressions (#660).

## [0.28.0] -- 2026-07-11

### Changed

- **CPS-IR-to-C backend is now the always-on codegen path**: the emitter
  that lowers a colored function's ANF/CPS IR directly to C -- under the
  ratified DK-threading ABI, covering the direct<->CPS boundary edges,
  letcont join points, and the load-bearing shift/reset case where a
  callee's shift is delimited by a caller's reset -- is no longer
  experimental. `emit-c` emits `__cps` bodies unconditionally, with no
  flag required. This release retires the last of its experiment-flag
  surface (#658), completing the graduation begun in the previous cycle.

### Removed

- **`--enable=cps-backend` flag surface fully removed**: with the backend
  always-on, the residual experiment-flag plumbing is gone.
  `--enable=cps-backend` now reports the standard unknown-experiment error
  (TUR-E0310) instead of an accept-and-warn shim (#658).

### Added

- **`--dump-direct-lowering-callers` metric**: new diagnostic counter
  reports the residual eviction and direct-dispatch caller populations
  reaching the direct-style delimited-control lowering, measuring progress
  toward retiring the legacy direct lowering (#659).

## [0.27.7] -- 2026-07-10

### Added

- **Eq/Show for every single-word collection element type**: `Vec`,
  `Set`, and `Map` are now `Eq`- and `Show`-able over any single-word
  element type, not just `:int` (#654).
- **Collection Show in the WASM REPL**: the Try Turmeric REPL renders
  bare collection results via `Show` instead of raw handles (#651).

### Changed

- **Delimited-continuation runtime relocated**: the DK runtime moves
  into place and the `reset`/`shift` and `call/cc` implementations
  close previously open gaps (#656).

### Fixed

- **Grounded tyvar ascriptions for string keys**: `Show[Set]` and
  `Show[Map]` over `cstr` keys now render the strings instead of raw
  values (#655).
- **Bare-head constrained instance dispatch**: the interpreter binds
  constraint tyvars for bare-head constrained instances at dispatch
  time (#652).

## [0.27.6] -- 2026-07-09

### Added

- **CPS-IR-to-C backend (experimental)**: new emitter lowers a colored
  function's ANF/CPS IR directly to C under a ratified DK-threading
  ABI, covering the direct<->CPS boundary edges, letcont join points,
  and the load-bearing shift/reset case where a callee's shift is
  delimited by a caller's reset. Gated behind `--enable=cps-backend`;
  off by default and neutral to the suite until a fixture opts in
  (#649).

## [0.27.5] -- 2026-07-09

### Added

- **Show instances for typed collections**: `Vec`, `Set`, and `Map`
  now have `Show` instances (in a new opt-in
  `stdlib/typeclass-show.tur`), and the REPL renders bare collection
  results as `[1 2 3]` / `#set{...}` / `#map{...}` instead of raw
  handles (#648).
- **Content-keyed `Set[A]`**: `Set`'s element API is generalized off
  `:int` and dispatches `Hash`/`MapKey` on the concrete element type,
  so `Set[cstr]`, `Set[Sym]`, and content-equal elements at distinct
  addresses now behave correctly. `Set[int]` output is byte-for-byte
  unchanged.

### Docs

- New plans for container Eq/Show element dispatch, boxed multi-word
  elements, and an owned `String` type; new reports on the WASM REPL
  Show-preload gap and the web REPL inline-C native gap.

## [0.27.4] -- 2026-07-09

### Added

- **Legible character literals**: `#\a`, `#\space`, `#\newline`,
  `#\u41`, and friends now read as ints holding the Unicode code
  point, matching Scheme-style character-literal syntax (#647).

### Docs

- Parser combinators tutorial refresh; new plan doc for legible
  character literals under `docs/upcoming/v1/`.

## [0.27.3] -- 2026-07-08

### Added

- **Stdlib preload for REPLs**: the WASM (Try Turmeric) REPL and
  `tur repl` now preload the stdlib on startup, so `(head ...)`,
  `(tail ...)`, and other stdlib forms are available at the prompt
  without a manual import.
- **Web deploy-gate CI**: a new CI workflow runs a Playwright smoke
  spec against the deployed web REPL to catch stale wasm /
  cache-version drift.

### Fixed

- **Try Turmeric stdlib gap**: the deployed web REPL was missing
  stdlib natives; the preload path and `sw.js` `CACHE_VERSION` bump
  restore parity with the CLI REPL.

## [0.27.2] -- 2026-07-08

### Added

- **Pure-reader admissibility (BR3b/c)**: by-ref aggregate params and
  transitive pure-reader borrow chains are now admitted in the
  catch-unwind trampoline.
- **Stackless catch-unwind aggregate returns**: support aggregate
  Result return types with correct box lifecycle and branching tails.

### Changed

- **Collection natives relocated into libturi** for interpreter /
  compiled-backend parity.
- **Effect-op seed locking** via mechanical coloring (F1) and the
  effect-rec probe (F4).

### Fixed

- **Interpreter buffer reclamation**: Vec, Set, and Map buffers are
  now reclaimed at env teardown (previously held for process
  lifetime).
- **HAMT delete**: refcount handling and double-free of shared
  siblings on lineage free.
- **Generator resume** no longer corrupted by intervening top-level
  evaluations.
- **Value-position if-branch** panic caused by miscompiled
  `(null) = ((void)0);`.
- **Panic / catch-unwind memory**: non-heap free of opaque panic
  payload, free-nonheap-object warnings, discarded Result box leak,
  and work-stack perform-capture on the panic path.
- **Escaping work-stack effect continuations** relocated during
  scratch promotion.
- **Interpreter**: list-concat crash and Option heap-payload unwrap.

## [0.27.1] -- 2026-07-07

### Added

- **Stackless catch-unwind** graduated to always-on. The compiled
  backend now models `catch-unwind` on the driver work-stack with
  panic-as-signal semantics (Phases C1/D1 through D3-G), surfaces the
  result as `(Result A B)` so `ok-val`/`err-val` infer, widens
  by-const-pointer aggregate param eligibility (BR3), and aggregates
  params in the group driver.
- **Van Laarhoven lenses**: nested-mapper dict dispatch lowered
  (Phases 1-3) via `forall-dict-pass`, which also graduates to
  always-on with multi-constraint dict-clone frame support.
- **Four HKT/forall experiment flags** graduated to always-on.

### Fixed

- **macOS-only dispatch-array overflow** from uninitialized main-body
  `EmitCtx`; zero-init on entry.
- **Panic-unwind box leak**: free the discarded catch-unwind Result
  box and the caught panic payload; free the work-stack perform
  capture on the panic path.
- **Async fiber stacks** are reclaimed on completion instead of held
  until env teardown.
- **Dict-clone result-type threading** for polymorphic methods.

### Docs

- Fix drifted sweet-exp toggle pairs in guides.

## [0.27.0] -- 2026-07-05

### Added

- **Manifest `:exports` accepts `#map{...}` literal syntax**. The
  `:exports` clause in `build.tur` now recognizes the `#map{k v ...}`
  data-literal form alongside the existing plain-map spelling, with
  dedicated diagnostics for malformed keys/values. See
  `docs/upcoming/exports-map-syntax-tighten-plan.md`.

### Fixed

- **REPL reload** rebuilt against the current wasm; `mise.toml` trimmed
  of stale entries.

### Docs

- Lens guide typo fixes.

## [0.26.6] -- 2026-07-05

### Added

- **turi value-pool: scratch/permanent split with escape promotion**
  (Phase A, #609). Bounds steady-state memory for a single long-lived
  `TuriEnv` (notebook-kernel pattern) without changing interpreter
  semantics. Split `TuriEnv.value_arena` into `value_scratch` (default
  alloc target) and `value_perm` (promoted survivors). Opt in with
  `turi_env_set_scratch_promotion`; at each `turi_eval` top-level
  boundary a Cheney-style two-pass deep copy relocates provably-safe
  escapees (scalars, strings, closures + captured frames/bindings/
  tyvars, structs) into perm, then rewinds scratch. Conservative bail
  on carrier-encoded bare-int pointers, live continuations/generators/
  handlers/futures, and scratch-resident refs. Off by default; the
  existing per-unit-env path is unchanged.

### Fixed

- **CI regen-snapshots `--check` now honors per-fixture `flags`
  files**. Previously ran `tur emit-c` without them, mis-reporting
  experiment-gated snapshots (e.g. `van-laarhoven-lens-wide-*`) as
  drift and threatening to regenerate them to empty.

### Changed

- **Docs**: the value-pool scratch-promotion plan is archived
  (executed); a follow-up
  `docs/upcoming/turi-value-pool-carrier-relocation-plan.md` captures
  the remaining carrier/live-C-state relocation tail.

## [0.26.5] -- 2026-07-05

### Added

- **Consumer monomorphization graduated; `vl-wide-mono` retired**
  (CM1-CM4). By-value HKT across the van Laarhoven lens boundary
  (Path B) is now unconditional; the `--enable=vl-wide-mono`
  experiment is gone. CM1 resolves each consumer's lens param to the
  full set of concrete lenses reachable at its call sites via a
  fixpoint. CM2 emits box-free consumer clones
  (`<consumer>__lens_<hash>`) for ambiguous (|set|>=2) params. CM3
  rewrites those call sites to the clones, dropping the lens arg.
  CM3-transitive specializes forwarding consumers through the call
  graph. CM4 gates composed lenses back to Path A via a
  `has_composed_lens` poison so the graduation is safe.
- **By-value propagation for composed van Laarhoven lenses** (CB1-CB5).
  A composed lens (`line-a-x`, whose body tails into another lens via an
  adapter lambda) and any consumer passed one now thread `(f a)` by value
  end to end on Path B -- no carrier box at any composition crossing. The
  resolve pass re-admits composed lenses and registers their nested lenses'
  `<lens>__mono` bodies; the poly-call emit redirects each nested
  application to the nested mono body; VBM2b mints a by-value twin of the
  adapter closure. `van-laarhoven-lens-wide-compose` runs by value with an
  `expected.c` codegen snapshot.
- **REPL terminal escape-sequence handling** for cursor / key events in
  `src/turi/repl.c`.

### Changed

- **Docs**: `docs/guides/lens-guide.md` now documents Path B as always-on
  with the simple-vs-composed distinction and the two Path A fallbacks
  (runtime-selected + composed lenses). Resolved `docs/upcoming/` plans
  moved to `docs/archive/`; the residual by-value-propagation slice for
  composed lenses is captured in
  `docs/upcoming/v2/van-laarhoven-composed-byvalue-plan.md`.

## [0.26.4] -- 2026-07-04

### Added

- **By-value van Laarhoven monomorphization: spec discovery** (VBM1,
  #605). Under `--enable=vl-wide-mono`, wide-functor lens call sites
  register per-concrete-functor specs the emitter will later drive.
- **Cross-procedural concrete-lens resolution** (VBM2a, #606). A new
  `mono_specs_resolve_program` pass joins each abstract lens spec to
  the concrete lens passed at every top-level call of its enclosing
  fn, collapsing abstract `(l g s)` pins to a resolved emit key.
  `--dump-mono-specs` prints both the abstract and resolved tables.

### Changed

- **CLAUDE.md: green-suite gate excised.** `bash tests/run.sh` is a
  signal, not a gate. A red suite -- intermediate or otherwise --
  never blocks a commit, PR, or calling a change done.

## [0.26.3] -- 2026-07-03

### Added

- **User-level experiments file** (UC-2/UC-3). A
  `~/.config/turmeric/experiments.tur` list enables experiments across
  projects; CLI `--enable=` and manifest `:experiments` still take
  precedence.

### Changed

- **`TUR_M7_HKT` feature gate retired** (#603). By-value HKT is the
  only path; the conditional carrier code and env-var branch were
  collapsed.
- **CI skips the CPM.cmake fetch** when running in CI environments,
  avoiding an unnecessary network dependency.

### Removed

- **`--allow-experimental` was retired** (UC-4). Enabling an experiment
  -- via `--enable=<name>`, a `build.tur` `:experiments` list, or
  `~/.config/turmeric/experiments.tur` -- is now itself the
  acknowledgment. The `TUR-W0060`/`TUR-W0061` lifecycle warnings fire
  whenever an experiment is enabled, with no way to silence them.
  Passing `--allow-experimental` is a hard error with a targeted
  "retired" message for one release; remove the flag from any scripts.

### Fixed

- **By-value Option/Result if-join and Result-parameter codegen** now
  produce correct C output (#601).
- **`-Wint-conversion` on HRT poly-calls** with pointer-class return
  types eliminated (#600).
- **Defstruct bracket fields** are no longer dropped when they follow
  a type-param vector (#599).
- **Method-level HKT tyvars** now ground through match-arm unification
  (#598).

## [0.26.2] -- 2026-07-02

### Added

- **Wide by-value van Laarhoven functors through the lens boundary**,
  behind `--enable=vl-wide-functor` (WF1).

### Changed

- **Syntax and LSP tweaks** across the main driver, stdlib docstrings,
  and vim highlighting.

### Fixed

- **Match-arm error path** no longer leaks linear-state snapshot
  buffers.
- **Wide by-value van Laarhoven functors** now report a targeted
  `TUR-E0309` diagnostic instead of miscompiling.

## [0.26.1] -- 2026-07-02

### Added

- **Constrained HKT + rank-2 `forall`, slices 1-4.** Kind annotations,
  constraint propagation, higher-kinded rank-2 quantification, and
  van Laarhoven lenses (`view`/`set`/`over`).

### Changed

- **Class-method result functor inferred from the receiver**, so
  `fmap`/`bind` sites no longer need explicit functor annotations.
- **Generic focus type flows through rank-2 `forall` lens arguments**,
  making van Laarhoven `view`/`set`/`over` inferable at use sites.
- **`tur run` improvements.**

### Fixed

- **Poly combinator element tyvar** grounded through the returned
  closure's application, unblocking inference of polymorphic
  combinators that return functions.

## [0.26.0] -- 2026-07-02

### Added

- **`interpret` CLI subcommand** and REPL `:explain` meta-command
  (native + web), rounding out the introspection story.
- **REPL clickable diagnostics (L3)** and stale-buffer tracking (L4)
  for the web REPL; L5 split out for follow-up.
- **CLI auto-suggest** for mistyped subcommands.

### Changed

- **structdef-retirement, slices 4 + 5 + DS-D.** Lower `:linear`
  defstructs and `defopaque` to record ADTs; remove the `TY_STRUCT`
  type representation and the dead struct-app registry / typedef
  chain; migrate single-occurrence param placeholders to named
  tyvars.
- **element-field plan (EF-1..EF-4).** Route `(arrow ...)`/`(-> ...)`,
  `(handler E V R)`, and `Session`/`project`/`Role` as struct/ADT
  fields; shelve `forall` as a field (design decision); reject bare
  descriptors + `Global` at that position.
- **`with` on narrowed multi-variant ADTs (CONV-S4N)** and unified
  struct/variant construction diagnostic wording (CONV-S6).
- **`cstr` byte primitives reimplemented in pure Turmeric.**
- **turi stdlib inline-C parity** for the list/map/vec/slice
  representation bridges.
- **Test-performance-optimization-plan phases 2 + 3** land, cutting
  suite wall-clock.
- **Parser combinator tutorial rewritten.**

### Fixed

- **Three parametric-ADT / codegen reports** archived after fix.
- **Option-monomorph mangler collision** over unresolved placeholders.
- **`ok`-construct float-payload-into-carrier-box value coercion.**

## [0.25.6] -- 2026-06-28

### Added

- **Web REPL multi-tab editor.** Drag-reorderable tabs, project
  download as zip, project load from zip, mobile overflow menu, and
  Playwright multi-tab specs.
- **Debugger upgrades.** DAP gains an in-frame expression evaluator
  and conditional breakpoints; LLDB pretty-printers for core types;
  VS Code Native Debugging configuration and integration docs.

### Changed

- **`defstruct`-as-`defadt` seam 4 graduation.** Force-lower blockers
  dropped from 212 to 60 across the suite via the seam-4 work
  (adt-recursive, hkt-ap-fn-in-container, Option/Result pointer-slot
  parity, by-value ADT return bridges).

### Fixed

- **Nested-carrier match losing concrete element type.**
- **stdlib `future`/`taskgroup` nested-function hoisting** so the
  GCC nested-function extension is no longer required; `str.tur`
  returns typed correctly.
- **macOS `ucontext_t` ABI mismatch** that made `TuriEnv` layout
  translation-unit-dependent.
- **Apple clang 17 `-Werror=int-conversion`** noise in generated C
  suppressed at the build-system level.

## [0.25.5] -- 2026-06-26

### Added

- **Explicit `#fx{...}` reader form for effect rows.** Phase 1 of the
  fx-row-syntax-rename-plan: `#fx{...}` is the new canonical spelling,
  while legacy bare `#{...}` and `@{...}` effect rows now carry
  provenance tags so elab can emit deprecation diagnostics (TUR-D0002 /
  TUR-D0003) on first consumption.
- **`tools/migrate-fx-rows.py`** automates the rewrite from the legacy
  spellings to `#fx{...}`; applied across the stdlib and the fixture
  suite.

## [0.25.4] -- 2026-06-26

### Added

- **PWA support and mobile split-view for the web REPL.** Service
  worker, install manifest, custom `_headers`, kill-switch SW for
  recovery, and a mobile split-and-PWA Playwright spec covering
  `/try`.

### Changed

- **Curly-infix `{a + b}` is now enabled in the default
  s-expression dialect, not just sweet-exp.** Reader-form change
  in `src/compiler/reader.c` with three new default-dialect
  fixtures (`curly-infix-default-{basic-add,mixed-vars,nested}`).
- **Contract types must use the explicit `#refine{var : T | pred}`
  reader form** instead of the former bare `{var : T | pred}`
  overload, which is freed up for curly-infix. Two new fixtures
  (`refine-basic`, `refine-in-defn`); reader-forms,
  contract-types, and syntax guides updated.

## [0.25.3] -- 2026-06-26

### Added

- **CONV-S1 struct/ADT convergence widens to parametric, fn-field, and
  pointer-field structs (#551, #554, #561, #566, #567).** `defstruct`
  lowering now handles by-value ADT products with nested aggregates,
  rc<ADT> field access, drop-glue, large-ADT pass-by-pointer ABI, typed
  `fn` fields, and parametric record-ADT field access with
  instance-head resolution.
- **Parametric ADT by-value monomorphisation flipped on (P2-P4, #559).**
  Wire crossings landed and the gate is on.
- **Heap-ADT auto-lowering + carrier bridges for parametric `:heap`
  (#568, #569).** Seam 3 of the CONV-S1 ABI work: typed-pointer ADT
  foundation plus auto-lowering and bridges.
- **B4 slice 2: wide by-value ADT closure params via heap-box carrier
  (#563).** Wide ADTs survive fat-closure boundaries.
- **Typed consumers for `Set` (#564) and `Eq[Cons]` via typed
  `(Cons__A *)` consumers (#553).**
- **libturi embed peripherals: Gaps 1-8 (#547, #549) + configurable
  include paths for embedders.**
- **Debugger Phase 4 + 5 (#548, #550).** Native source maps for
  `emit-c`; native type-name audit and gdb pretty-printers.
- **Typed native registration (#552).** Curated facades over embedder
  natives now type-check.
- **TUR-W0040: warn on eval-mode unknown call heads (#560).**

### Changed

- **`Map` is now a non-transparent heap struct backed by a HAMT pointer
  (#555).**
- **Macro args elaborated before expansion; list-macro quote vs
  syntactic-symbol fixes; nested vec literals collapse to a runtime
  vec.**

### Fixed

- **By-value Result/Option field access aggregate-to-pointer cast
  (#557).**
- **Multi-param struct-app spine preservation for param/return types
  (#556).**

### Docs

- **Godot binding guide (G5 prep) + typing follow-ups plan
  (G6.1-G6.3).**
- **Windows cross-compile bootstrap plan (#562).**
- **Cross-script calls section (T4.A starter); Tier 3 and Tier 4 plans
  archived as shipped.**

## [0.25.2] -- 2026-06-24

### Added

- **Interpreter debugger (`tur debug`) + DAP server (#543, #544, #545).**
  Three-phase debugger: source-spans audit and coverage gate, an
  interactive REPL-style stepper over the interpreter, and a Debug
  Adapter Protocol server so editors can attach.
- **By-value ADT representation for leaf-scalar products (CONV-S1/B3,
  CONV-S2/S4, #540, #541, #546).** Struct/ADT convergence:
  record-style variants, keyword construction, match-on-struct, and
  by-value layout for single- and multi-variant ADTs without the
  carrier indirection.
- **Mise plugin.** Install Turmeric via `mise`.
- **Try Turmeric (web playground) overhauled.** PWA manifest +
  apple-touch-icon, non-scrolling mobile layout, editor/console
  persistence across reload, iOS safe-area + focus-zoom handling, and
  a legible mobile Examples dropdown.

### Changed

- **turi interpreter perf.** Env-owned value-arena pool (#537),
  pooled inline-C escaping buffers, and coroutine-stack tracking
  (#539).
- **Spice `__ok` / `__err` migration complete.** All 13 spices
  (json, sqlite, png, rtaudio, osc, template, wav, postgres, valkey,
  ...) now use the canonical Result builders; umbrella plan archived.

### Fixed

- **By-value struct/ADT results now survive the closure ABI (#538).**
- **Invalid C initializer when let-binding a by-pointer struct param
  (#542).**

## [0.25.1] -- 2026-06-24

### Added

- **Typed inline-C Result/Option builders.** New preamble helpers
  `tur_ok_ptr` / `tur_ok_int` / `tur_err_ptr` / `tur_err_int` /
  `tur_some_ptr` / `tur_some_int` / `tur_none` let an inline-C body
  return real `(Result T E)` / `(Option T)` values that flow straight
  into stdlib `ok?` / `ok-val` / `some?` / `unwrap` -- no hand-rolled
  struct, no `:ptr<void>` escape hatch, and no `(int64_t)(intptr_t)`
  cast at the call site. The `_int` and `_ptr` suffixes spell out the
  payload's cast direction; a `_Static_assert` pair pins the
  Result/Option byte layout to the stdlib shape. New guide:
  [`docs/guides/inline-c-results-guide.md`](docs/guides/inline-c-results-guide.md).
- **Struct ergonomics: auto-bound constructor, keyword args, with-update
  (#535).** `defstruct` now generates an auto-bound constructor; struct
  literals accept keyword arguments; new `with` form copies a struct
  with selected fields replaced.

### Fixed

- **Parametric struct fn-field call-through carrier-ptr ABI mismatch
  (#534).** Calling a function-typed field on a parametric struct no
  longer trips the carrier-vs-by-value ABI seam.
- **`(. obj field args)` receiver-first dot routing (#533).** Method-style
  dot syntax now routes through the receiver type's method table even
  when arguments follow the field name.
- **Honor `#[used]` on single-file / whole-program build path (#532).**
  The `#[used]` attribute is no longer dropped when the build is invoked
  on a single file or in whole-program mode.

## [0.25.0] -- 2026-06-23

### Removed

- **`throw` / `try` / `catch` deleted.** The Phase S4 exception forms
  are gone end-to-end: elab arms (`elab_throw`, `elab_try_catch`),
  `EX_THROW` / `EX_TRY_CATCH` IR enum tags and struct members, codegen
  arms in `emit_expr.c` / `emit_stmt.c`, the borrow-check arm, the
  interpreter eval cases plus the `DK_TRY_BODY` / `DK_TRY_CATCH`
  driver continuations, `turi_native_throw` / `make_throw_val`, and
  the `TUR-D0002` deprecation warning. Use `result<T,E>` for
  recoverable failures and `panic` / `catch-unwind` for unrecoverable
  ones. Runtime helpers (`tur_panic_with`, `tur_catch_unwind`, the
  `setjmp`/`longjmp` plumbing) stay -- they're load-bearing for
  `panic`. See
  [`docs/archive/throw-deprecation-plan.md`](docs/archive/throw-deprecation-plan.md).

### Changed

- **Interpreter fiber rejection now flows through a new
  `TURI_REJECTION` value tag.** `(await ...)`, `with-timeout`, and
  `task-cancel` produce `TURI_REJECTION` instead of throwing; callers
  observe rejections with `(error? r)` / `(error-message r)` instead
  of `(try ... (catch [e] ...))`. The new tag is distinct from
  `TURI_ERROR` so binding a rejection in `(let [r (await ...)] ...)`
  does not trigger the interpreter's universal error short-circuit.

### Fixed

- **Opaque / struct tyvar binding lost through closure arg position
  (#530).** Type-variable bindings for opaque newtypes and structs
  now survive being threaded through a closure argument position.
- **Typed inline lambdas as macro arguments now splice verbatim
  (#529).** Typed `(fn [...] ...)` literals passed to a macro are
  preserved through quasiquote expansion instead of being re-parsed
  as raw symbols.

## [0.24.3] -- 2026-06-23

### Added

- **STM finished on TL2 fine-grained locking (#528).** The half-built
  per-TVar-mutex commit path is replaced with a complete TL2
  (Transactional Locking II) discipline in both the reference runtime
  and the inline-emitted runtime compiled programs use. Reads are
  lock-free with version revalidation; commits lock only the stripe
  buckets covering the write set, bump a global version clock, and
  publish writes under a per-TVar lock bit. `retry()` parks on a
  bucket-filtered cond. New `stm-stress` (8 threads x 500 contended
  increments, TSan-clean) and `stm-retry-wakeup` fixtures.
- **Stackless delimited control and `call/cc` on the work-stack
  (#526, #527).** Heap-bound single-operand black-box forms (N3) and
  stackless `shift`/`shift0`/`call/cc` (N4) move off the re-entrant C
  frame onto the driver work-stack, removing two synchronous
  `turi_call` re-entry sites.
- **Experimental feature flag mechanism (`--enable=<name>`) (#524).**
  New `EXPERIMENTS[]` registry in `src/runtime/experiments.c` with
  `name`, `summary`, `plan_path`, `introduced`, `expires_at`,
  `lifecycle`, and `opt_global` fields; lifecycle warnings TUR-W0060
  and TUR-W0061 fire on use. See
  [`docs/guides/experimental-flags-guide.md`](docs/guides/experimental-flags-guide.md).

### Changed

- **`try`/`catch`/`throw` deprecated.** Use sites emit a deprecation
  note pointing at result/option-based error handling. The legacy
  fixture is parked under `_legacy-throw/`.

### Fixed

- **Parametric `defstruct` + fn-typed field gaps (#523, #525).** Type
  arguments for parametric structs are now inferred from fn-typed
  fields, closing the remaining defstruct holes.
- **Multi-param projection + functional dependencies for associated
  types (#522).** Assoc-types-2 Part A lands; multi-parameter
  projection composes with fundeps.
- **Workspace `:members` seed transitive cmake-deps (#521).** Enclosing
  workspace member lists now feed transitive cmake dependencies on
  spice builds.

## [0.24.2] -- 2026-06-23

### Changed

- **Dead `-X` feature-flag guards removed (#520).** Now that every
  gated feature is always-on (v0.24.0), the trivially-true
  `if (g_*_enabled)` guards in elab/emit have been unwrapped and the
  dead `if (!g_*_enabled)` branches deleted. The
  `extern bool g_*_enabled` symbols stay defined in `globals.c`, so
  any external code linking against them keeps working.

### Docs

- Added the experimental-flag-mechanism plan under
  `docs/upcoming/v1/` and refreshed the bundled web/WASM build.

## [0.24.1] -- 2026-06-23

### Added

- **`ref<T>` auto-drop on scope exit (#519).** Affine `ref<T>` values
  are now automatically dropped when they go out of scope, removing
  the manual `(drop! r)` boilerplate previously required.
- **`TurChannel` session type runtime in codegen preamble (#517).**
  Session-typed channels now have their supporting runtime emitted as
  part of the standard codegen preamble.
- **Spice-level vendored C sources via `:c-sources` / `:c-includes`
  (#516).** `build.tur` manifests can now declare bundled C source
  files and include directories that build with the spice.

### Fixed

- **Auto-loaded `defmodule` stdlib macros now promoted to global
  visibility (#515).** Macros defined inside auto-loaded stdlib
  modules are visible at the use site without manual re-import.
- **Always-on session/linear fixtures (#518).** Fixture failures that
  surfaced when session types and linearity went always-on are
  resolved.

## [0.24.0] -- 2026-06-23

### Changed

- **All 16 `-X` feature flags are now accept-and-warn no-ops; every
  gated feature is on by default (#TBD).** The flags
  (`-Xlinear`, `-Xsubstructural`, `-Xunique-types`, `-Xgadt`,
  `-Xunion-types`, `-Xintersection-types`, `-Xeffect-types`,
  `-Xcontracts`, `-Xsessions`, `-Xdynamic-vars`, `-Xcallcc`,
  `-Xsized-types`, `-Xdata-literals`, `-Xjson-reader`,
  `-Xschema-reader`, `-Xsymbols`) are still recognized for source
  compatibility, but each one emits `TUR-W0050` and otherwise does
  nothing. Existing `build.tur` files and CI invocations keep
  compiling unchanged. See
  [`docs/guides/compiler-flags-guide.md`](docs/guides/compiler-flags-guide.md)
  for the removal list and
  [`docs/upcoming/v1/drop-x-flags-plan.md`](docs/upcoming/v1/drop-x-flags-plan.md)
  for the plan.
- **`--strict-effects` no longer auto-on with effect types.** When
  `-Xeffect-types` was a real flag, passing it also enabled
  `--strict-effects`. Effect typing is now always on, but
  `--strict-effects` stays opt-in -- code without explicit `forall [e]`
  annotations no longer gets the nudge unless you pass
  `--strict-effects` explicitly.
- **Partial features go always-on at their current completion level.**
  `-Xunique-types` (UT0--UT3) and `-Xsized-types` (SZ0--SZ9; static
  checking covers folded-constant sizes, runtime assertions cover
  open-expression sizes) now light up for every program. Existing
  not-yet-shipped-bit diagnostics (`TUR-E0260`, etc.) continue to fire
  unchanged.

### Internal

- 283 fixture `flags` files containing only `-X` tokens deleted; the
  rewritten `docs/guides/compiler-flags-guide.md` is now a short
  diagnostic-flag reference plus a "Removed Feature Flags" pointer
  table.

## [0.23.3] -- 2026-06-23

### Deprecated

- **All `-X` feature flags will become accept-and-warn no-ops in the next
  minor release (0.24.0).** Every feature currently gated behind an `-X`
  flag will be unconditionally on; `-X<name>` will still be recognized for
  one full minor line and emit `TUR-W0050` instead. Downstream `build.tur`
  files that pass `-X` flags will continue to compile without modification.
  See `docs/guides/compiler-flags-guide.md` and
  `docs/upcoming/v1/drop-x-flags-plan.md` for the full removal list and
  rationale.

## [0.23.2] -- 2026-06-23

### Added

- **Turmeric Version Manager (`tvm`) (#511).** Install, use, and switch
  between Turmeric releases from the command line.
- **Uniqueness types: UT2 inference + UT3 stdlib patterns (#513).**
  Inference for uniqueness annotations plus stdlib coverage of the
  common patterns.

### Changed

- **R2 dispatch-side constraint-var mapping collapsed to a shared kernel
  (#508).** One audited mapping path instead of per-site variants.

### Fixed

- **`ap` fn-in-container by-value monomorphization (#507).** Regression
  coverage added; plan archived.

### Internal

- **IT4 close-out: `defdata`-as-union fixture; stale parity-check path
  fixed (#514).**
- **Sized-types marked complete; docs and plans archived (#512).**
- Archived turi-parity-post-v1, HKT dispatch options tradeoff, and
  turi-interpreter-gap-closure plans (#506, #509, #510).

## [0.23.1] -- 2026-06-22

### Fixed

- **Route value-side carrier<->concrete recovery through one chokepoint
  (#505).** Consolidates the value-side recovery paths so remaining
  carrier-crossing edge cases land on a single, audited transition rather
  than ad-hoc per-site rewrites.
- **Fix output in Try Turmeric (web REPL).**

## [0.23.0] -- 2026-06-22

### Added

- **Track C / U3 advances to 2/4.** sqlite TStmt landed end to end.
- **Track C / U6 closes.** Typed variadic c-dsl builders finished; v2 plan
  filed for the valkey follow-up.

### Changed

- **`(list ...)` is element-type polymorphic via `tcons-of` (#473, #488).**
  Float heads and other non-int elements now flow through the classic
  list surface without ascription.
- **Applied type constructors accepted in `defdata` constructor fields
  (#483).**

### Fixed

- **Carrier<->by-value bridging across constrained-generic instance
  dispatch (#490, #491, #493-#495, #497, #503, #504).** Ascribed and
  no-int-instance paths, float reinterpretation, by-value field access
  through ascribed receivers, return-dispatched by-value-struct method
  elements specialized into generic loops, struct-headed applied-instance
  matching, and unascribed carrier-helper reads in constrained instances
  all resolved.
- **HKT cata over function-typed carriers (#489, #499).** Fn result
  threading and match-arm payload capture fixed; segfault when the
  function carrier's argument is itself a function eliminated.
- **Captureless algebra arms and letrec self-capture under fat closures
  (#500-#502).** Mixed fn/value carrier env-struct collisions carved;
  captureless arms no longer thicken through fat-closure carriers;
  letrec self referenced from a nested closure now captures correctly.
- **By-value parametric struct field layout (#481, #482).** Fields embed
  the aggregate; constrained generics returning parametric containers
  monomorphize correctly.
- **Nested generic by-value construct in constrained instance bodies
  (#480);** parametric `:heap` struct field extraction no longer
  collapses to the carrier (#479); recursive `(Cons A)` specs no longer
  hijacked by carrier-erasure ascription (#498); control-form result
  temps thread the by-value carrier-ABI aggregate (#492).
- **Multi-index opaque cross-parameter unification for sized matrices
  (#476).**
- **Constrained instance method dispatch on pointer-carried element
  types (#475).**
- **`type_name` emit-path leak on composite type diagnostics (#477).**
- **CI: fmt bootstrap, REPL spice loading, and turi gates greened up
  (#474).**

## [0.22.0] -- 2026-06-20

### Added

- **Turi interpreter parity (Track E closed, #398).** EX_CONS_LIST support
  in the interpreter (#435) plus 7 turi fixture fixes (#457) bring the
  tree-walking evaluator to parity with the compiled path.
- **`#[used]` attribute -- retain a defn with external C linkage (#467).**
  Keeps defns reachable only via raw `extern` (Arrow release fns, qsort
  comparators, signal handlers) from being demoted to `static`. Unblocks
  the `frame` spice's group/interop/reshape suites.
- **Classic Lisp list surface in stdlib (#470).** `car`/`cdr`/`null?`
  alongside the existing `head`/`tail`.
- **Migration diagnostics + guide for legacy `:int`-pointer struct
  forms (#466).**

### Changed

- **End-to-end monomorphization (Track A) lands (#444).** Carrier->by-value
  bridging completes across constrained generics, HOFs, existential
  pack/open, option/result accessors, and tail calls into inline-C ADT
  helpers. Touches dozens of PRs (#395-#469); audit floor moves to 0
  carrier deref-copies on the by-value path.
- **TCO lands in ABI specs.** Eq[Vec]/Eq[Map]/Eq[Set] rewritten as
  pure-Turmeric TCO'd loops (#400, #424); tail calls now bridge to
  inline-C carrier-ABI helpers at the return site (#415, #416).
- **MutableMap retyped to honest `(MutableMap K V)` (#396);** its carrier
  bridge is retired.
- **Vec inline-C producers monomorphized to typed pointers** (Track A
  bucket A follow-ups, #391/#393).

### Fixed

- **Spice-uplift wave.** Two separate-compilation codegen blockers (#465);
  prelude monomorphized specs no longer emit `static` with external
  linkage; file-scope inline-C include guards no longer corrupted by the
  dedup pass (`stats` spice unblocked); ECS E2d typeclass dispatch gaps
  closed (#405); StorageOps bounded-wrapper heterogeneous monomorphization
  gap closed (#447, #448); MutableMap typed-pointer producer
  monomorphization (#411); `time` importable as a module (#410).
- **`-lm` is now linked unconditionally (#471),** so pure spices can use
  libm symbols (`sqrt`/`fabs`/`sin`/...) without a fake cmake-dep.
- **letrec self-recursive float accumulator no longer collapses to int
  carrier (#469).**
- **HKT instance-method spec emitted when consumed by a match scrutinee
  (#468);** Applicative `ap` preserves fn type through polymorphic
  constructors (#438); layer-4 by-value `<|>` / selection-body shape
  (#442).
- **Closure capture of bindings referenced inside open/pack/dispatch
  (W3, #464);** letrec carrier self-call typing and fresh `vec-new` arg
  unification (W1/W2, #463); NULL-deref in `elab_lookup_ctor` on
  malformed defdata field type (#462).
- **Existential pack/open of multi-field struct payloads via heap-boxing
  (#420);** existential `open` dispatch through packed witnesses (#452);
  by-value struct payload rejected in constrained pack with a clear
  diagnostic (#455).
- **Return-type unification: cstr-commit/integer-body (TUR-E0708, #454)
  and float-vs-non-float register-class (TUR-E0707, #453) mismatches now
  rejected.**
- **CT macro evaluator: `map` and nested macros now work in splices
  (#406).**

## [0.21.0] -- 2026-06-15

### Added

- **Sized types ON by default (#392).** `-Xsized-types` flips on, with
  SZ8 recovering size indices from variables and struct projections
  (#367).
- **`EX_CONS_LIST` interpreter support** plus an autolink-duplicate
  fix.

### Changed

- **Vec migrated to the `:heap` typed-pointer ABI (#377).** Carrier-based
  Eq[Vec] retired (audit 98 -> 70, #393) and Vec inline-C producers
  monomorphized to typed pointers (Bucket A, #391).
- **M5 Option C: by-value twin redirect for carrier stdlib accessors
  (#369).** Carrier->concrete return deref for by-value instance
  methods; the EX_ASCRIBE CK_CONCRETE->CK_CARRIER bridge is gone
  (#368).
- **Pure-Turmeric fat-closure dispatch finishes de-inline-C.** Ascribing
  `:int` / `:ptr<void>` carrier to a fn type marks it boxed.
- **cfnptr `:usize` / `:isize` lower to `size_t` / `ptrdiff_t`**, and
  `ptr<const-T>` lowers to `const T*`.

### Fixed

- **Parametric `:linear` opaques now enforce single-use.** A
  `(defopaque Name [T] :int :linear)` previously compiled a double-use
  cleanly under `-Xsubstructural` because the four TY_APP construction
  sites hardcoded `copy_kind = CK_COPY`, dropping the head's substructural
  qualifier. `src/compiler/types.c::propagate_app_discipline` now lifts
  `:linear` / `:affine` from the head onto every application node;
  invoked from `type_app`, both `substitute_*_app_type`, and
  `elab_types.c::type_expr_from_form`. Regression:
  `tests/fixtures/errors/parametric-linear-double-use/`. Unblocked the
  ECS `WriteCap<T>` capability surface (Phase I of the ECS prereq
  plan).
- **Generic-dict dispatch re-resolution under `--interpret` (#386)**,
  carrier-fallback instance method dispatch under `--interpret` (#381),
  and return-dispatch on constrained type variables (with deserialize
  support).
- **Carrier-source Result/Option bridge for sub-word payload types.**
- **Generic-of-generic carrier callees emit via a carrier-relay
  closure.**
- **`make-struct` cstr->int64 carrier field bridge cast (#374)** and
  phantom-typeparam lowering for mixed phantom/by-value structs.
- **c-fn-ptr typedefs emitted to the module header before use (#375).**
- **Workstealing-balance deque race (#372).**
- **`Serializable[int].deserialize` avoids signed left-shift UB.**

### Removed

- **EX_ASCRIBE CK_CONCRETE->CK_CARRIER bridge (#368).**
- **Dead carrier-int `vec-eq-loop`** following Eq[Vec] retirement.

## [0.20.0] -- 2026-06-11

### Added

- **Variadic HKT rows (#330)** -- `^&` row-kinded parameters land all six
  layers; backs the ECS variadic for-each surface.
- **Associated type members on typeclasses (#327)** -- single associated
  type with dictionary-free type-level projection.
- **Application image dumps** -- serializable continuations + image
  dump/restore via the AI1/AI2/AI4/AI5/AI6 plan; resource-reacquisition
  hooks for replay; see [image-dumps-guide.md](docs/guides/image-dumps-guide.md).
- **Sized types for GADTs** -- SZ6-SZ8 type-level index with
  cross-parameter unification; see
  [sized-types-guide.md](docs/guides/sized-types-guide.md).
- **Turi interpreter parity (TI1-TI4)** -- `EX_LETREC` + `EX_SET_FIELD`;
  generators (`gen`/`yield`/`gen-next`/`gen-done?`); abortive delimited
  control + STM.
- **Sweet-exp manifests + codemod (#322)** -- `build.tur.sweet` manifest
  support (SW0-SW8); `fn`-type-colons codemod extended to
  `definstance`/`defclass`/`defprotocol`.
- **`build/` output directory** -- `tur build` routes artifacts under
  `<root>/build/{obj,bin,lib}/`; configurable via `--build-dir` /
  `TUR_BUILD_DIR` / `:build-dir`.
- **Deep transitive `:cmake-deps`** -- recursive walk + fetch-site
  cycle/conflict detection.
- **bare-fat-result-monomorphization (Phase B)** -- non-tail float
  register-class via per-call-site monomorphization.
- **ECS prereq compiler fixes** -- typeclass-constrained `defn` parsing,
  `F_TYPE_ANN` unquote in `substitute_params`, top-level `(def name init)`
  emission into `__tur_module_def_init` constructor, CBLOCK quasiquote
  auto-wrap, top-level `(do ...)` splicing.

### Changed

- **Phase 21: serial-shift capture grammar generalized** to do-sequences
  and beyond.
- **TUR-E0706: hard error for non-capturable serial-shift contexts
  (#331)** -- replaces the silent-0 miscompile.
- **`fn`-type leading colons rejected (Phase 4)** -- inside `(fn ...)`
  type expressions.
- **Per-declaration dedup for file-scope inline-C blocks.**
- **Stdlib consolidations** -- type-erasure-cleanup and hkt-consolidation
  tracks closed.

### Fixed

- **Multi-line inline-C formatting + stdlib layout (#324).**
- **Project-mode spice builds** -- fat-closure codegen + `defstruct`
  typedef emission + RC runtime preamble (T1-T11) all landed.
- **Archive churn sweep** -- 19 resolved plans/reports moved to
  `docs/archive/history/`.

## [0.19.1] -- 2026-06-06

### Added

- **Editor color themes** -- ships Emacs (`turmeric-dark-theme.el`), Vim
  (`vim-syntax/colors/turmeric-dark.vim`), and VS Code
  (`vscode-syntax-ext/themes/turmeric-dark-color-theme.json`) dark themes
  tuned for Turmeric syntax.
- **Reversible name mangling (#301)** -- executes plan stages T3/T5/T6/T7/T8;
  kebab/snake coexistence and operator mangling are now round-trippable.

### Changed

- **`>>>` rewritten to polymorphic typed form** -- removes the float-specific
  `compose-float-*` helpers in favor of a single typed `compose` arrow.
- **Fixture-codegen decoupling (#299)** -- Phase 1 of the fixture-churn-paydown
  plan strips ~200 KLOC of preamble noise from `tests/fixtures/*/expected.c`,
  so future codegen tweaks no longer ripple through hundreds of snapshots.

### Fixed

- **Project-mode `defstruct` typedef missing** -- `emit_module` now hoists
  struct typedefs ahead of the inline-C uses that reference them, fixing
  project-mode spice builds.
- **File-scope inline-C block emit ordering** -- elements in inline-C blocks
  now emit in source order, fixing a latent miscompile.
- **`^fat` let-binding of `ptr<void>` fat closure** -- partially addressed in
  `emit_module`; remaining cases tracked in `docs/reported/` (#305, #306).

### Docs

- Guides refresh for v0.19 (arrows, effects, error-handling, GADTs, httpd
  middleware, session types, compiler flags, delimited control).
- Docs archive curated: completed plans moved under `docs/archive/history/`.

## [0.19.0] -- 2026-06-05

### Added

- **Typed closure invocation ABI (#276)** -- `TUR_APPLY{0..4}_T` macros thread
  declared `fn` argument and return types to the C invocation site, retiring
  int64-only erasure through `TUR_APPLYn` / fat-shims. Closures taking or
  returning `:float`, `:ptr`, `:bool`, and `:cstr` now codegen with correct C
  signatures end-to-end.
- **`Category` typeclass and Kleisli `ArrowZero` (#290)** -- `stdlib/arrow.tur`
  gains an honest `Category` with `id`/`compose` and `ArrowZero` with
  `zero-arrow`; the full `Arrow` hierarchy consolidates into `stdlib/arrow.tur`.
- **stdlib `Functor`/`Monad`/`Alternative` instances for `Option` (T1)** --
  `Option` gains `Functor`, `Applicative`, `Monad`, and `Alternative` instances;
  `Result` gains `Bifunctor`; `do-m`/`for`/`fmap`/`bind`/`alt-or` work over
  stdlib optionals and results without manual imports.
- **`min` and `max` prelude macros** -- available in every module alongside
  `when`/`cond`/`for`/`unless` without an explicit import.
- **Codegen snapshot CI guard and `regen-snapshots` recipe (#298)** -- `tur run
  regen-snapshots` bulk-regenerates fixture snapshots; a CI gate prevents
  snapshot drift from landing undetected.

### Changed

- **Injective Turmeric-to-C name mangling (#275)** -- `-` maps to `_hy`, `_` to
  `_un`, `/` to `_sl`; the scheme is now injective and reversible. Manual
  `extern` declarations in spice inline-C must be updated to use the new
  mangled names.
- **`(fn [...] ...)` leading colons deprecated (TUR-D0001)** -- `(fn [:float]
  :float)` now emits a deprecation warning; the bare-identifier form `(fn
  [float] float)` is the target. A codemod swept `stdlib/` and `tests/` in
  #270.
- **`tur build` compiles dep modules for shared-library spices** -- library
  spices with no `main` (e.g. tourist) previously skipped dep-module header
  generation. Dep headers are now always emitted; dep `.c` files are excluded
  from the link via the new `n_own` parameter.
- **Project-mode prelude auto-load** -- `tur build <dir>` auto-loads the stdlib
  prelude in per-module compile paths, making `when`/`cond`/`for`/`unless`
  available inside `defmodule` bodies without explicit imports.
- **Local typeclass instances shadow stdlib on erased dispatch** -- when
  `.method` dot-dispatch on an erased receiver is otherwise ambiguous and
  exactly one matching instance is user-defined, that local instance is selected
  instead of failing with `TUR-E0020`.

### Fixed

- **Fat-closure / typed-SF dispatch (cluster)** -- resolved interacting closure
  codegen bugs: poly-closure inner dispatch result erasure, `copy_kind`
  initialization, two-level SF return miscompilation, `Vec<SF>` fold blockage,
  `int`-to-`ptr<void>` carrier casts, let-bound SF type-check routing, and
  `^fat` param emission in inline-C bodies (#276, #283, #286, #287, #292, #293,
  #294, #295, #296).
- **Recursive `defn` return type in `defmodule` (#291)** -- `F_TYPE_ANN`
  wrapper was not unwrapped for recursive defn return-type slots, causing
  spurious type errors in self-recursive functions inside a `defmodule`.
- **`definstance` idempotency (#278)** -- repeated `(load ...)` of a file
  containing `definstance` no longer duplicates the instance.
- **macOS codegen miscompile: uninitialized `arg_poly_fn` (#274)** -- silent
  wrong-code on macOS in the poly-function path; covered by a new fixture.
- **`definstance` ergonomics (#284)** -- stdlib helper resolution gains better
  hints, a load fallback path, and corrected effect annotations.

## [0.18.0] -- 2026-06-02

### Added

- **`tur/httpd` standard middleware library (M0-M8)** -- headers (M0), request
  logging + composition (M1/M8), cookies and `Set-Cookie` (M2), urlencoded +
  JSON body parsers (M3), CORS and HTTP Basic Auth (M4/M5), multipart/form-data
  (M7), and body-size, rate-limit, and static-file middleware. Includes
  `docs/guides/httpd-middleware-guide.md`.
- **`tur/httpd` async server (A0-A4)** -- async accept loop and `await`
  primitives (A0/A1), Track-M middleware composes over async handlers (A3),
  in-flight cap with throughput fixtures and `docs/guides/httpd-async-guide.md`
  (A4).
- **`?` query operator and CPS error handling (#187)** -- `?` short-circuits on
  `Result` errors with a dedicated `TUR-E0001` message; `*-must`/`*-expect`
  route through `tur_panic_with` so `catch-unwind` can intercept them;
  `--no-contracts` strips runtime contract checks; `--lint-panic`
  soft-deprecates `*-unwrap` call sites.
- **`catch-unwind` and panic handling on the compiled path (R2 + R6c, #189)**
  -- compiled programs can intercept panics via `catch-unwind`; the
  effect-handler/continuation panic semantics are now normative.
- **CPS transform unification and full `call/cc`** -- continuation-passing
  phases consolidated; `call/cc` works end-to-end on the compiled path.
- **Intersection and union types** -- new type-system constructors for
  combining and alternating type constraints.
- **MCP LSP integration (#173)** -- ships an MCP server that exposes
  Turmeric's LSP for editor agents.
- **`parse-check` subcommand and syntax guide (#182)** -- standalone
  syntax-validation pass and a comprehensive surface-syntax guide.
- **Tourist routing composition** -- `tourist/routing` (sibling spices repo)
  adds `url-map!`, `cascade!`, `cascade-with!`, and `req-full-path` for
  mounting sub-apps and cascading on configurable statuses. See
  `docs/guides/tourist-routing-guide.md`.
- **GitHub Codespaces entry points (#174)** -- homepage and README link
  directly to a configured Codespaces environment.

### Changed

- **`^fat` ABI markers replace sentinel-capture workaround (#190)** --
  function-pointer marshalling now uses explicit `^fat` markers instead of
  the prior closure-sentinel detour; spice code referencing the old path
  may need to migrate.
- **Compiler: variadic-rest function-pointer args (V0-V2)** -- function-typed
  variadic rests are cast correctly at call sites, removing per-call
  adapter shims.
- **Compiler: inline-C by-value struct params (DS0-DS2)** -- call sites for
  inline-C functions taking by-value structs now match the C signature
  directly.

### Fixed

- **Neoteric bracket chaining and curly-infix operator detection (#188)** --
  `f(x)(y)` chains parse correctly under sweet-exp and `{a + b}` operator
  scanning is no longer confused by adjacent identifiers.
- **Sweet-exp polish** -- residual indentation and reader edge cases
  addressed across guides and fixtures.

## [0.17.0] -- 2026-06-01

### Added

- **`tur/httpd` HTTP/1.1 server (H1-H7)** -- `stdlib/httpd.tur` adds a full
  HTTP/1.1 server built on `tur/reactor`: blocking thread-per-connection (H1),
  reactor-backed listener (H2), bounded worker pool with shared fd queue (H3),
  persistent connections (H4), routing DSL with method guards and path parameters
  (H6), and middleware composition via `httpd-call` (H7).
- **`reactor-run-fibers` local fiber driver (F1-F8)** -- `tur/reactor` gains a
  single-reactor fiber group for cooperative concurrency without a separate
  scheduler thread.
- **Data literals `#map{...}` / `#set{...}` (DL0)** -- compile-time reader
  dispatch lowers `#map{:k v ...}` and `#set{a b ...}` to typed map/set
  constructors; composes with neoteric and curly-infix in sweet-exp files.
- **`tur/schema` runtime validation (SC0-SC7)** -- `stdlib/schema.tur` adds
  `HasSchema` typeclass, typed decode, `Validation` applicative, and
  `Functor`/`Applicative`/`Alternative` instances via transparent newtype (SC7).
  Includes `docs/guides/schema-guide.md`.
- **First-class runtime symbols `:Sym` (SYM0/SYM1/SYM4)** -- `-Xsymbols` enables
  interned `:Sym` values, `sym->str`, `sym=?`, and dynamic interning via
  `stdlib/sym-dynamic.tur`. Includes `docs/guides/symbols-guide.md`.
- **Typed `Map[K V]` surface (TMS2-TMS5)** -- unified macro accessors for
  content-keyed maps; `float32`/`float64` map keys via `MapKey` carrier typeclass
  (WKC); string maps via `smap-of` lowering (GMK2).
- **`#json(...)` compile-time reader macro (JR0)** -- parses a JSON literal at
  compile time and lowers it to typed map/vec constructors.
- **`let*` sequential-binding form** -- each binding sees previous bindings in
  scope; complements `let` and `letrec`.
- **`^fat` closure markers (A#1)** -- `^fat` parameter annotation triggers
  fat-closure auto-shim generation; `^fat` return-position marker propagates
  the annotation into nested lambdas.

### Changed

- **`^unsafe-multishot` annotation removed (MS4)** -- the annotation is no longer
  needed; remove any remaining `^unsafe-multishot` from spice code.
- **`#map`/`hamt-of` lowering unified (GHE4+GHE5)** -- all content-keyed map
  builders now use per-`K` fn-value specialization; `smap-*` APIs are removed.
  Migrate `smap-of` call sites to `#map{...}` or `hamt-of`.

### Fixed

- **Nested-closure captures in `#{Unsafe}`/`handle` bodies (KB-IDIOM-1)** --
  captures from outer scopes are correctly threaded into `unsafe`/`handle`
  body environments.
- **Cross-module private-defn C symbol collision (CC0-CC2)** -- private `defn`
  symbols in different modules no longer collide in emitted C.
- **Unnecessary parentheses in codegen** -- `BIN_INFIX`, `PREFIX_UNARY`, and
  `VARIADIC_FOLD` expressions no longer emit spurious wrapping parentheses.
- **Generic dict eq dispatch** -- `dict-eq?` correctly dispatches through
  `Hash`/`Eq` typeclasses for all registered key types.

## [0.16.0] -- 2026-05-30

### Added

- **`tur/reactor` event loop (R1-R8)** -- `stdlib/reactor.tur` adds a
  lightweight single-threaded reactor for multiplexing file descriptors,
  timers (one-shot and interval), OS signals (`reactor-add-signal`), and
  cross-thread channels (`reactor-add-chan`) without requiring the fiber
  scheduler. Backed by epoll on Linux and kqueue on macOS/BSD. Includes
  `docs/guides/reactor-guide.md` and `docs/tur-httpd-plan.md`.

### Changed

- **Phase F poly-dispatch extended to unsigned narrow ints** -- `uint8`,
  `uint16`, and `uint32` now use the concrete-cast fast path for
  `(forall [a] (-> a a))` calls, avoiding the `int64_t` carrier
  round-trip on x86-64.

### Fixed

- **`tur new` scaffold Justfile** -- scaffolded recipes now pass the
  correct targets (`tur build .`, `tur test tests/`, `tur check src/`);
  the CI contract was updated to `clean check test`.
- **`handler` name shadowing (FH2)** -- the `handler` special form no
  longer shadows higher-order parameters named `handler` when a local
  binding is in scope.
- **Reactor fixture leak markers** -- nine reactor test fixtures are
  tagged `requires.no-leak-check` to suppress false-positive
  LeakSanitizer reports from process-lifetime callback closures.
- **LS5 spice resolver memory leak** -- `cmd_run` now frees the
  strdup'd directory strings before freeing the `spice_inc_dirs` array.

## [0.15.0] -- 2026-05-30

### Added

- **`tur run` Justfile task runner** -- `tur run <recipe>` executes Justfile
  recipes with deps, params, `{{ interpolation }}`, `set` directives, and
  dotenv. `tur new` gains `--kind lib|bin`, `--author`, `--license`, CI
  workflow scaffolding, and a standard Justfile.
- **`tur fmt` formatter** -- `tur fmt [paths...]` reformats `.tur`/`.tur.sweet`
  files in-place with recursive directory walking; `--check`, `--stdout`,
  `--stdin`, and `--lang` flags; idempotent on all stdlib sources.
- **First-class handler values (FH0-FH7)** -- `(handler E [params] k body)`
  creates a portable handler value; `(with-handler hv body)` applies it;
  `(compose-handlers h1 h2)` composes disjoint-effect handlers (h1 outer).
  Multi-effect handler types `(handler #{A B} V R)` supported.
- **Explicit lifetime syntax (LS0-LS5)** -- `&'a T` and `&mut 'a T` in type
  annotations; implicit quantification; borrow return types; inter-procedural
  borrow-escape checking (TUR-E0105/E0106).
- **`-Xsized-types` flag** -- promoted to a real compiler flag (implies
  `-Xgadt`); ships type-level size indices (`SizedVec n`), static compile-time
  size checking (TUR-E0260), size inference, and `--dump-sizes` diagnostic.
- **`letrec`, `named-let`, and `(define ...)`** -- `letrec` supports self-
  recursive and mutually-recursive bindings; `named-let` desugars to
  tail-recursive `letrec`; `define` provides `let*`-style body-position
  binding in `defn`, `fn`, `let`, and `do`.
- **Typed variadic rest parameters for user-defined types** -- `& rest :T`
  now fully type-checks opaque, struct, ADT, and type-application rest
  arguments; unknown type names are hard errors; `:int`-and-cast workarounds
  are no longer needed.
- **Cross-module ABI specialization (J1-J7)** -- `tur build <dir>` performs
  a two-pass build that emits owned-clone bodies in the owner module and
  extern forward-decls in borrowers; `.tur-abi-cache/index` persists across
  incremental builds.
- **`any` boxing and if-guard narrowing (TY2/TY3)** -- `(box :any v)`,
  `(cast :T v)`, and `(if (is? :T v) ...)` narrowing in control flow.

### Fixed

- **Memory leaks in composite-type diagnostics** -- `type_name()` result
  strings are now arena-managed; error paths are ASan/LSan-clean.
- **Codegen: Clang int-to-pointer warnings** -- spurious `int<->pointer`
  casts eliminated from all generated C.
- **Runtime autolink path resolution** -- prefix-installed builds resolve
  absolute `-I`/`-L` SDK paths correctly.

## [0.14.6] -- 2026-05-28

### Docs

- **Guide polish across stdlib documentation** -- copy edits and
  formatting fixes for the C integration, cellular-automata comonad,
  custom-effects, HAMT, HKT, and type-erasure guides.

### Internal

- **`scripts/wait-for-release.sh`** -- helper script for maintainers
  to watch a release workflow run from the CLI after pushing a tag.

## [0.14.5] -- 2026-05-28

### Fixed

- **`tur uninstall` rejected names from older `tur list` output** -- prior
  `tur list` rendered entries as `name-version` with no separator (e.g.
  `tur-notebook-0.1.0`), so users copying that string into `tur uninstall`
  got "is not installed". Uninstall now falls back to splitting on a
  trailing `-<digit>...` suffix and matching against the recorded version.
- **`tur list` separates name and version with a space** -- kebab-cased
  package names like `tur-notebook` are no longer visually fused with
  their version (`tur-notebook 0.1.0` instead of `tur-notebook-0.1.0`).

## [0.14.4] -- 2026-05-28

### Fixed

- **`*args*` empty in user-defined `main`** -- a user-defined zero-arg
  `main` now receives `(argc, argv)` at the C level and populates
  `g_tur_args` from the process argv before user code runs, matching the
  synthesized-main path. Previously `*args*` was always empty in
  user-main programs (which broke spice CLI parsing).
- **`tur install` git fetch errors include cache path + hint** -- when
  `pkg_git_fetch` fails on a previously-cloned cache directory, the
  error now reports the destination path and suggests `git -C ... status`
  / `rm -rf ...` to inspect or discard the dirty cache.

## [0.14.3] -- 2026-05-28

### Fixed

- **`tur install` global SDK path resolution** -- prefix-installed builds
  (e.g. Homebrew) now resolve absolute `-I`/`-L` paths to the Turmeric SDK
  when compiling spices that use `-lturi`; previously, the relative paths in
  `__tur_autolink__` failed when the working directory was not the source tree.
  Resolution order: `$TUR_SDK_ROOT` override, then walking up from the
  executable to locate `share/turmeric`.

### Changed

- **VSCode syntax extension** -- corrected the Markdown injection grammar so
  Turmeric code fences in Markdown files highlight correctly.

## [0.14.2] -- 2026-05-28

### Fixed

- **`tur install` spice dependency resolution** -- `tur install` now fetches each
  `:spices` dep declared in `build.tur` into the global spice cache and adds their
  `src/` directories as `-I` paths when building binaries; previously, spices with
  transitive deps (e.g. `tur-notebook` depending on `ansi` and `png`) failed to
  build with "module not found" errors.

## [0.14.1] -- 2026-05-28

### Fixed

- **`tur --help` missing package commands** -- `tur install`, `tur uninstall`,
  `tur list`, and `tur upgrade` were dispatched correctly but absent from the
  help output; they now appear under the "package management" section.

### Docs

- **`tur/hash` API reference** -- docstrings for `hash-int` and all
  sized-integer variants (`hash-int8`/16/32/64, `hash-uint8`/16/32/64)
  added to the stdlib API reference and web REPL doc lookup.

## [0.14.0] -- 2026-05-28

### Added

- **ABI specialization (Phases C--I)** -- the compiler now performs typed ABI
  specialization for typeclass methods, concrete structs, and inline-C bodies.
  Integer hashing, bitwise ops, and pass-by-ptr structs use unboxed calling
  conventions (C/D); function-pointer fields in concrete structs are unboxed (E);
  inline-C bodies opt in via `__TUR_TY_<NAME>__` macros (G); typeclass dispatch
  routes directly to the instance implementation rather than going through the
  generic slot (H). New `--emit-abi-trace` flag reports applied specializations (I).

### Changed

- **Web playground promoted to public beta.**

### Fixed

- **Typeclass and stdlib fixes (KB-021--034)** -- GADT HKT constraint unification
  for `equal-cong` (KB-022); typeclass dispatch ABI mismatch for struct-typed
  instances (KB-021); `rc.tur` HKT instances now dispatch on `rc<T>` rather than
  `ptr<void>` (KB-027); orphan checker credits built-in primitive types to their
  home module (KB-030); `session.tur` return-type annotation corrected (KB-029);
  `gvzip-with` in `gadt-vec.tur` now takes a typed function parameter (KB-034).

- **HKT/Clone fixture conflicts** with auto-loaded stdlib typeclasses resolved.

- **macOS Clang compatibility** -- `run.sh` adds `-Wno-error` gates for
  `int-conversion` and `incompatible-function-pointer-types` when building with
  Clang, fixing CI failures on macOS Xcode 15+.

## [0.13.0] -- 2026-05-27

### Added

- **Spice-aware REPL (RP0--RP8)** -- `tur build --shared` emits a dlopen-able `.so`;
  `tur repl` auto-discovers and dlopens the enclosing spice, binds all exports as
  callables at the prompt, and refreshes them via `(reload)` or `--watch`. Error paths
  surface actionable hints for the three most common failure modes.

- **`#rx` regex literals and `#name"body"` reader macros** -- `#rx"pattern"` compiles
  to a regex literal at read time; `#name"body"` is a general reader-macro hook. `re`
  union helpers round out the regex API.

- **Variadic rest parameters** (`& rest :type`) -- functions now accept an unknown number
  of same-type trailing arguments; rest is a cons-list of the declared type and is `nil`
  when absent.

- **Currying** (`curry` macro, effects, rank-2) -- `curry` macro, algebraic effects in
  curried bodies, and rank-2 support (CY3+CY4).

- **Tuple2--Tuple5 built-in structs** -- `Tuple2` through `Tuple5` are now pre-defined
  in the compiler; pointer-type slots are handled correctly.

- **Sized-primitives mixed-width arithmetic (TUR-E0042)** -- mixed-width expressions over
  `i8`/`i16`/`i32`/`i64`/`u8`/`u16`/`u32`/`u64` now elaborate without manual casts.

- **`vec-of` macro** -- construct a `Vec` from a literal element list.

### Changed

- **Contract macros removed; `--no-auto-stdlib` added** -- the old `assert!`/`require!`/
  `ensure!` contract macros are no longer loaded by default; `--no-auto-stdlib` suppresses
  automatic stdlib injection entirely.

- **macOS release binary is now arm64-only** -- the release matrix is
  `linux-x86_64`, `linux-aarch64`, and `macos-arm64`.

### Fixed

- **Aggregate Carrier ABI (ACB Phase 1 + 3)** -- ACB Phase 1 audited call sites for
  struct-carrying aggregates; Phase 3 completes the carrier-to-struct bridge in
  `EX_ASCRIBE` so aggregate returns through ascription work correctly.

- **GADT `Vec` rename, `Clone` typeclass, `Functor` collision** -- `Vec` GADT renamed to
  avoid shadowing the stdlib type; `Clone` typeclass added; `Functor` name collision
  between stdlib and user definitions resolved.

- **Spaced compound type annotations** -- `f : (-> int int)` and compound annotations
  with internal spaces now elaborate correctly through the full pipeline.

- **`?` operator** -- missing `__tur-q-is-err?` and `__tur-q-ok-val` stdlib helpers
  added so the `?` early-return operator functions correctly.

## [0.12.0] -- 2026-05-25

### Changed

- **Typed stdlib modules now use canonical names**
  - The old `t*` module names were dropped in favor of unprefixed modules such as `vec.tur`, `map.tur`, `option.tur`, `result.tur`, `pair.tur`, `list.tur`, `grid.tur`, `zipper.tur`, `set.tur`, and `mutmap.tur`
  - Compiler preloads and synthesized structural-equality helpers were updated to use the new module and helper names
  - Tests, benchmarks, and generated docs were refreshed to use the unprefixed APIs throughout

- **Docs layout cleanup**
  - `drop-typed-prefix-plan.md` moved into `docs/archive/`
  - Several roadmap docs were promoted out of `docs/upcoming/` into the main `docs/` tree

## [0.11.0] -- 2026-05-25

### Added

- **`defalias` for primitive type aliases**
  - New `(defalias Name :primitive-type)` form for defining named aliases of primitive types such as `:int` and `:float`
  - Function parameter and return annotations now resolve these aliases during elaboration
  - Added coverage for basic aliases, float aliases, and invalid alias targets

- **New guides**
  - `frame-guide.md` -- using the `tur-frame` spice for in-memory columnar dataframes and Arrow interop
  - `tur-logic-guide.md` -- miniKanren-style relational programming with `tur/logic`

### Changed

- Documentation archive reorganized under `docs/archive/` and `docs/archive/history/`, with the archive index refreshed
- The miniKanren tutorial was renamed to `minikanren-1-relations-and-queries.md` to make room for a multi-part guide series

## [0.10.0] -- 2026-05-25

### Fixed

- Higher-order function return types now preserve their full `TY_FN` payload through elaboration and calls, so let-bound functions returned from other functions remain callable instead of degrading to a non-callable shell type.
- The signal spice no longer needs `requires.typecheck-skip`; `signal/core.tur`, `signal/dsp.tur`, `signal/envelope.tur`, and `signal/synth.tur` all typecheck cleanly against the current compiler and signal API surface.

## [0.9.0] -- 2026-05-22

### Added

- **LSP intelligence (LD0-LD4)** (#67)
  - `textDocument/hover` -- returns a Markdown snippet with the symbol's type signature and `;;;` docstring
  - `textDocument/definition` -- returns the source location for the symbol under the cursor
  - `textDocument/completion` -- prefix-filtered `CompletionItem` list from the symbol index; also completes stdlib module paths inside `(import ...)` forms
  - New LSP helpers: `lsp_scan_docs` (re-scans `;;;` blocks without invoking the compiler), `tur_collect_symbols` (elaboration-based symbol index)

- **`Show` typeclass (SI0-SI2)** (#68)
  - `Show [bool]` -- returns `"true"` or `"false"`
  - Fixed `Display [int]` -- was `"<int>"`, now decimal via `snprintf %lld`
  - Fixed `Debug [int]` -- was `"<int>"`, now `"int(N)"` format
  - Fixed `Display/Debug [ptr<void>]` -- correct `ok(N)` / `err(N)` / `Result::ok(N)` / `Result::err(N)` formatting
  - `derive-show` macro: generates a `Show` instance for any struct from a field descriptor list
  - New compile-time built-ins powering `derive-show`: `vec?`, `symbol-name`, `dot-sym`, `str-append`

- **`name : type` annotations** -- the parser now accepts a space before the colon (`name : type`) in addition to the existing `name :type` form

- **Datum comments** (`#;`) -- `#;expr` suppresses the next form without removing it from the source, matching standard Scheme/Racket convention

- **Sweet-expression syntax** (`#lang sweet-exp`) -- opt-in indentation-sensitive syntax; `#lang sweet-exp` at the top of a file (or a `.tur.sweet` extension) enables full t-expression + neoteric + curly-infix mode

- **Devcontainer** -- `.devcontainer/devcontainer.json` for one-click VS Code Remote / GitHub Codespaces setup

- **Spices directory page** -- `tools/genspices.py` generates `docs/html/spices/index.html` from the `turmeric-spices` README (local `../turmeric-spices/` or GitHub fallback) with full Turmeric syntax highlighting; `just spices` runs it; `just docs` now depends on it

- **New guides**
  - `lsp-guide.md` -- setting up the LSP server with VS Code, Neovim, and Emacs
  - `advanced-type-system-rationale.md` -- design rationale for HKTs, GADTs, session types, and sized types
  - `arrows-guide.md` -- composable `Arrow` abstractions and the `>>>` / `***` / `&&&` combinators
  - `sandboxing-guide.md` -- capability-restricted interpreter environments and step-fuel limits

### Fixed

- LSP server: fixed a crash when the client sent a request before the workspace root was known
- `Display [int]` / `Debug [int]` / `Display/Debug [ptr<void>]` now produce correct output (see Show typeclass above)

### Changed

- Guide code examples reformatted throughout to follow the new Clojure-style indentation rules and sweet-exp style guide (Guides, API Docs, and Spices pages all updated)
- Documentation reorganised: several plan documents moved to `docs/archive/`

## [0.8.0] -- 2026-05-22

### Added

- **Language Server Protocol (LSP)** (#62)
  - `tur lsp`: stdio-based LSP server with hover, go-to-definition, diagnostics, and completion
  - `tur check --json`: machine-readable diagnostic output for editor integration
  - VSCode extension updated to launch `tur lsp` and wire hover/definition providers

- **Sandboxed eval (SB0–SB4)** (#65)
  - `turi_env_new_sandboxed()`: capability-restricted interpreter environment
  - `TuriCaps` bitmask (`TURI_CAP_IO`, `TURI_CAP_FFI`, `TURI_CAP_INLINE_C`, `TURI_CAP_ASYNC`, `TURI_CAP_UNSAFE`, `TURI_CAP_IMPORT`)
  - Step-fuel limit (`turi_env_set_fuel`) and max-depth guard (`turi_env_set_max_depth`)
  - `import` and inline-C call sites blocked in sandboxed environments
  - New API: `turi_env_allow`, `turi_env_deny`, `turi_env_has_cap`

- **Lazy sequences (LZ0–LZ1)** (#63)
  - `Seq[A]` pull-based lazy sequence type with short-circuit support
  - Builders: `seq-from-list`, `seq-from-range`, `seq-repeat`, `seq-iterate`
  - Transforms: `seq-map`, `seq-filter`, `seq-take`, `seq-drop`, `seq-flat-map`, `seq-zip`
  - Consumers: `seq-to-list`, `seq-fold`, `seq-for-each`, `seq-count`, `seq-first`
  - `Range` type with constructors and set-algebra operations (`range-intersection`, `range-gap`, `range-span`, `range-encloses`, etc.)
  - Stdlib in `stdlib/seq/core.tur`, `stdlib/seq/builders.tur`, `stdlib/seq/transform.tur`, `stdlib/seq/consume.tur`, `stdlib/range.tur`

- **9 new stdlib modules** (#64)
  - `stdlib/json.tur` -- JSON parse/emit
  - `stdlib/csv.tur` -- CSV read/write
  - `stdlib/fs.tur` -- filesystem operations
  - `stdlib/path.tur` -- path manipulation
  - `stdlib/env.tur` -- environment variables
  - `stdlib/process.tur` -- subprocess spawning
  - `stdlib/re.tur` -- POSIX regular expressions
  - `stdlib/term.tur` -- terminal control (ANSI colours, cursor)
  - `stdlib/digest.tur` -- hashing (SHA-256, MD5)

- **Doctest framework (D0–D5)** (#58)
  - `tools/doctest.py`: extract and run `;;; Example:` blocks from stdlib docstrings
  - `tools/run-doctests.sh`: stamp-cached test runner with SKIP support for interpreter-incompatible modules
  - `just doctest` target; `just test` extended to include doctests
  - Fixed incorrect docstring examples in `stdlib/math.tur` and `stdlib/async_pipe.tur`

- **Emacs major mode** (#61)
  - `emacs/turmeric-mode.el`: syntax highlighting, indentation, and `M-x turmeric-run` for `.tur` files

- **Spice `subdir` support**
  - Spice manifests now accept a `subdir` key for monorepo sub-packages

- **New guides**
  - Generators guide (`docs/guides/generators-guide.md`) -- generator state machine design and usage
  - CLI args guide (`docs/guides/cli-args-guide.md`) -- structured argument parsing with `stdlib/args.tur`
  - Cloudflare deployment guide (`docs/guides/cloudflare-deployment-guide.md`) -- deploying the web REPL to Cloudflare Pages

## [0.7.0] -- 2026-05-21

### Added

- **Sized types (SZ0–SZ1)** (`-Xsized-types`) (#50)
  - `StaticInt` and size arithmetic (`static-int-add`, `static-int-mul`, `static-int-eq`) for phantom size annotations
  - `SizedVec` -- length-indexed vector; `sized-vec-new`, `sized-vec-push`, `sized-vec-get`, `sized-vec-set`
  - `SizedBuf` -- flat byte/word buffers with heap and stack allocation dispatch; `sized-buf-alloc`, `sized-buf-stack`, `sized-buf-get`, `sized-buf-set`
  - `SizedMatrix` -- sized 2-D matrix with row/column shape annotations; `sized-matrix-new`, `sized-matrix-ref`, `sized-matrix-set`
  - `SizedBitVec` -- compact bit array with size annotation; `sized-bitvec-new`, `sized-bitvec-get`, `sized-bitvec-set`, `sized-bitvec-popcount`
  - Stdlib in `stdlib/sized.tur`, `stdlib/sized-buf.tur`, `stdlib/sized-matrix.tur`, `stdlib/sized-bits.tur`
  - Full user guide: [docs/guides/sized-types-guide.md](docs/guides/sized-types-guide.md)

- **Literal match patterns** (Phase S4) (#50)
  - Match arms now accept integer, boolean, float, and string literals directly (no wrapping constructor required)
  - Emits an `if`/`else-if` chain for primitive scrutinees; integrates with the existing ADT elaboration path

- **`async-race` and `with-timeout`** in the `turi` interpreter (#50)
  - `async-race`: race two futures; the first to resolve wins and the loser is cancelled
  - `with-timeout`: run a future with a deadline; cancel and throw on expiry

- **Tail call optimization (TCO)** in `turi` interpreter (#50)
  - Self-tail-recursive and mutually-tail-recursive functions no longer grow the call stack
  - Tail calls through `let` bindings and `do` blocks are optimized

- **`catch-unwind`** boundary in `turi` interpreter (#50)
  - `setjmp`/`longjmp` panic boundary exposed as `EX_CATCH_UNWIND`; `panic?` native predicate

- **Weak pointer upgrade returns Option** (#50)
  - `(weak-upgrade r)` now returns `(some value)` on success and `(none)` on dangling -- previously returned a raw value or zero

- **Args parser stdlib** (`stdlib/args.tur`) (#50)
  - Builder-pattern CLI argument parsing: flags (`--verbose`), options (`--input=file` or `--input file`), positional args, and arbitrarily nested subcommands
  - `args/spec-new`, `args/spec-flag`, `args/spec-option`, `args/spec-subcmd`, `args/parse`, `args/get`, `args/positional`

- **Math stdlib** (`stdlib/math.tur`) (#50)
  - Thin wrappers around libm: `sqrt`, `fabs`, `floor`, `ceil`, `round`, `pow`, `log`, `log2`, `exp`, `sin`, `cos`, `tan`, `atan2`, `hypot`

- **Bits stdlib** (`stdlib/bits.tur`) (#50)
  - `bit-shr` (unsigned right shift), `println-float` (float with precision), and related bitwise helpers

- **Per-subcommand help strings** (#50)
  - `tur build --help`, `tur run --help`, `tur eval --help`, etc. now print usage for each subcommand

- **Performance comparison suite** (`performance-comparison/`) (#50)
  - Multi-language benchmark harness comparing C, Turmeric (compiled), `turi` (interpreted), Rust, Clojure, Racket, and Python
  - `tur --interpret file.tur` flag runs programs through the tree-walking interpreter without compiling
  - Benchmarks cover numerical computation, data structures, string processing, concurrency, I/O, and recursion

- **Cross-language validation framework** (`validation/`) (#50)
  - Python validation scripts that run the same benchmark across languages and verify identical results
  - `validate_fibonacci.py` checks correctness of Fibonacci across all five languages

- **Tier 3 worker pool** (#45)
  - Persistent interpreter processes for test fixtures; dramatically reduces per-test process-spawn overhead

- **`emit_effects.c`** -- effects codegen extracted from `emit_expr.c` into a dedicated translation unit (#50)

- **EAVT / Datalog database tutorial series** (`docs/guides/datalog-*.md`, `examples/datalog/`)
  - Four-part guide: EAVT concepts, minimal implementation, query API, B-tree indexing
  - Five progressive example programs in `examples/datalog/`: `minimal.tur`, `indexed.tur`, `query.tur`, `blog.tur`, `datalog.tur`

- **New and expanded guides**
  - Performance guide (`docs/guides/performance-guide.md`) -- numerical, data structures, concurrency, memory, recursion, I/O, benchmarking methodology
  - Sized types guide (`docs/guides/sized-types-guide.md`)
  - Building for the Web with Emscripten (`docs/guides/web-emscripten-tutorial.md`)
  - Structs guide (`docs/guides/structs-guide.md`)
  - Web continuations tutorial (`docs/guides/web-continuations-tutorial.md`)
  - Dual Turmeric / sweet-expression syntax toggle throughout all guides (`check-guide-pairs.py`)
  - Substantially expanded: threading, STM, session types, HKT, tidal/scscm cookbook guides

### Fixed

- ADT match regression: literal match path no longer incorrectly triggers for ADT matches on unannotated parameters
- Memory leaks and stale codegen snapshots from perf-comparison branch
- `weak-dangling` test updated to reflect Option-returning `weak-upgrade`

## [0.6.0] -- 2026-05-19

### Changed

- Documentation refresh and cleanup
- Regenerated stdlib API reference

## [0.5.0] -- 2026-05-19

### Added

- **Session types -- binary (SS0–SS4)** (`-Xsessions`) (#38, #36, SS0a–SS3c)
  - `Session[P]` type; `make-session`, `send`, `recv`, `close` channel operations
  - `Choose`/`Branch` for internal/external choice; `choose-left`/`choose-right`/`offer`
  - `Rec` equirecursive protocols (co-inductive equality with seen-set guard)
  - Duality checking (`dual(P)`) and protocol-progress enforcement via linear-type machinery
  - Session delegation (protocol ownership transfer) and session subtyping
  - Typed timeout channels (`recv-timeout`, `TY_TIMEOUT`, `Timeout` protocol constructor)
  - C codegen: `TurChannel` struct; synchronous rendezvous via pthread condvars
  - Debug builds embed initial protocol name as `const char* dbg_proto`
  - Error codes `TUR_E0210`–`TUR_E0212`; `tur explain` entries

- **Session types -- multi-party (SS5–SS8)** (`-Xsessions`) (#39, #40, #41, #42)
  - `defprotocol` global protocol declaration with role list and interaction forms
  - `(-> From To MsgType)`, `(choice From [label branch ...])`, `(loop label body)`, `(continue label)`
  - Well-formedness checks (undeclared roles, non-guarded recursion): `TUR_E0223`
  - `(project G R)` type annotation: compile-time projection of a global type onto a role
  - Honda/Yoshida/Carbone projection algorithm (`src/compiler/elab_global.c`); validated by `tools/project.py`
  - Projection failure diagnostic `TUR_E0220`; role/projection mismatch `TUR_E0221`/`TUR_E0222`
  - `make-protocol` allocates N `Role[G, R]` endpoints (one per declared role)
  - `send-to`/`recv-from` route messages through a shared N-party lock-based router
  - Stdlib multi-party templates in `stdlib/session.tur`: `three-way-handshake`, `coordinator`, `ring`
  - Tutorials: Two-Phase Commit and OAuth-Style Auth Flow in `session-types-plan.md`
  - Full user guide: [docs/guides/session-types-guide.md](docs/guides/session-types-guide.md)

- **Dynamic vars (DV0–DV4)** (`-Xdynamic-vars`) (#44)
  - `defdynamic` top-level form declares typed thread-local cells with a root value
  - `binding` form pushes per-thread override frames; cleanup via `__attribute__((cleanup))`
  - Dynamic-var `set!` mutates the current thread's top binding frame
  - `TY_DYNVAR` type kind; `DynVarEntry` struct; codegen using `pthread_key_t` + linked-frame stack
  - `spawn-conveying`: spawn a thread with a snapshot of the parent's current binding frame
  - Stdlib common vars in `stdlib/dynvar.tur`: `*log-level*`, `*locale*`, `*random-seed*`, `*current-module*`
  - Error codes `TUR_E0600`–`TUR_E0605`, `TUR_W0600`; `tur explain` entries
  - Full user guide: [docs/guides/dynamic-vars-guide.md](docs/guides/dynamic-vars-guide.md)

- Datalog database tutorial (#35)

### Fixed
- Scheduler multithread codegen snapshot (#63906e87)

## [0.4.0] -- 2026-05-17

### Added
- Algebraic effects with delimited continuations and handler syntax (#25)
- WASM threads planning and infrastructure

## [0.3.2] -- 2026-05-17

### Changed
- Syntax highlighter improvements
- Documentation cleanup and reorganisation

## [0.3.1] -- 2026-05-17

### Changed
- Homepage layout and copy updates
- Documentation improvements

## [0.3.0] -- 2026-05-17

### Added
- GADTs -- generalised algebraic data types with full elaboration and codegen support (#24)
- Linear types -- linearity constraints enforced by the type system

### Fixed
- Homepage horizontal scroll on mobile (#23)

## [0.2.0] -- 2026-05-16

### Added
- Package manager -- CPM-based dependency management (#22)
- Effect rows -- row-polymorphic effect types
- Substructural and uniqueness types
- Linear types (initial support)
- "Solve this" button in the web REPL

## [0.1.0] -- 2026-05-14

### Added
- Higher-ranked types -- rank-N polymorphism (#18)
- GADTs -- initial implementation (#21)
- Arrows -- generalised computation abstractions
- Structural equality -- deep equality for all types (#17)
- Serialisable continuations -- capture and restore delimited continuations (#16)
- Contracts -- `require!`, `ensure!`, `invariant!`, and `assert!` macros
- Set literals -- `#s(...)` reader syntax
- HAMTs -- persistent hash-array-mapped tries
- STM -- software transactional memory
- Comonads -- comonad typeclass and standard instances
- Numeric types -- fixed-width integers and floats with explicit cast operators
- Auto-formatter -- source code pretty-printer
- Async/await -- fibre-based structured concurrency (#10)
- Unsafe effects -- escape-hatch unsafe block form (#11)
- HKT -- higher-kinded types, kind inference, and type application syntax
- Docstrings -- `;;;` doc-comment standard and `doc` macro
- Markdown Turmeric block syntax highlighting in the web REPL

## [0.0.4] -- 2026-05-09

### Added
- Algebraic effects infrastructure (v1) with delimited continuations
- Capability-passing effects (v1 effect system)
- Exceptions -- `try`/`throw` via `setjmp`/`longjmp`
- Defer expressions -- unified runtime-list-on-frame model
- Multi-file support and mutual recursion across files
- Async/await foundation -- fibre context switching (x64/arm64 asm)
- Compile-time macro evaluation via procedural elaboration
- `cond` as a variadic `defmacro` (removed built-in `elab_cond`) (#4)
- miniKanren-style logic programming example and guide
- Snake game example project
- Phases 0–19 of the core compiler (parsing, elaboration, CPS lowering, codegen)
