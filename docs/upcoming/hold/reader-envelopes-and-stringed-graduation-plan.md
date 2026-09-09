# Reader Envelopes: `stringed` Graduation + Literate `.tur.md`

Status: plan (feasibility assessed against the tree, not yet implemented).
Introduced: 2026-09-08
Supersedes: `reader-layers-decommission-and-at-exp-literate-plan.md` (never
committed; its literate design put literate mode on the base-dialect axis,
which this plan replaces -- see "Why literate is neither a layer nor a base").

## Overview

Two pieces, independent of each other:

1. **Graduate the `stringed` layer.** `#s"..."` becomes an always-on reader
   macro, so the only thing needed to use owned Strings is
   `(load "stdlib/string.tur")`. The `LANG_LAYERS[]` table empties; the token
   survives one migration window in `GRADUATED_LAYERS[]` as a warned no-op.
2. **Add a third reader axis -- the *envelope* -- and put literate `.tur.md`
   on it.** An envelope is a source-to-source pass that runs *before* base
   dialect selection and yields ordinary Turmeric source. It composes with
   every base rather than replacing one, which is exactly the property
   literate mode needs and which neither existing axis provides.

Part 3 keeps the `turmeric/at-exp` base-dialect research from the superseded
draft, because it is real work that is still wanted and because it is the
clean counterexample that makes the axis distinction concrete: at-exp *is* a
base dialect, literate is not.

---

## The three axes

Today the reader has two axes:

| Axis | Selected by | Cardinality | Mechanism |
|---|---|---|---|
| **Base dialect** | `#lang <base>` or extension | exactly one | `ReaderType` (`diag.h:388`), branches inside the reader + `sweet_preprocess` |
| **Layer** | `#lang <base> <layer>*` | a set | `LANG_LAYERS[]` bitset, `reader_hook` registers a `#`-dispatch |

This plan adds a third:

| Axis | Selected by | Cardinality | Mechanism |
|---|---|---|---|
| **Envelope** | file extension (`.tur.md`) | at most one | source-to-source pass producing base-dialect source |

The three are orthogonal: an envelope wraps a base, and layers ride along
regardless. `foo.tur.md` containing a ```` ```turmeric/sweet ```` fence is
envelope=literate, base=sweet, layers={}.

### Why literate is neither a layer nor a base

The instinct that literate mode must work with **both** `turmeric` and
`turmeric/sweet` is right, and it rules out both existing axes:

- **It cannot be a `LANG_LAYERS[]` layer.** The only power a `LAYER_READER`
  row has is its `reader_hook` (`lang_layers.h:38`), which is handed a
  `ReaderMacroRegistry` and can do exactly one thing: register a `#`-dispatch
  macro. It never sees the source text. Literate mode is a whole-buffer
  transform, so expressing it as a layer would mean inventing a third
  `LangLayerKind` whose hook has a completely different signature and runs at
  a completely different point -- a table that shares a name and nothing else.
  CLAUDE.md's layer contract ("flips on a `#`-dispatch ... additive and
  commutative") would no longer describe half its own rows.
- **It should not be a base dialect either** -- which is what the superseded
  draft proposed. `ReaderType` is a single field (`diag.h:437`); a
  `READER_LITERATE` base is mutually exclusive with `READER_SWEET`, so a
  literate file could only ever hold plain s-expressions. Recovering
  sweet-exp would take a `turmeric/literate-sweet` base, and then
  `turmeric/literate-at-exp`, `turmeric/literate-neoteric`... the
  combinatorial explosion the base axis exists to avoid.

The envelope axis is the shape the requirement actually describes: *one* new
concept, `N` bases x 1 envelope, no product table.

### The design trick that makes it cheap: blank, don't extract

The obvious implementation of a literate pass is "extract the code blocks and
concatenate them," which shifts every byte and forces an offset map
(`SweetMap`-style) so diagnostics point back at the `.tur.md`. Composing that
map with the sweet-exp map for a sweet-based literate file is real work.

Instead: **overwrite prose bytes with spaces, keep newlines, keep code bytes
in place.** The output buffer is byte-for-byte the same length as the input,
with identical newline positions, so every offset, line, and column is already
correct in original coordinates. Consequences:

- **No envelope offset map at all.** `diag_register_file` keeps the original
  `.tur.md` as the file contents and every span already points at the right
  line and column. No `xform_map`, no composition with `SweetMap`.
- **The base dialect composes for free.** `sweet_preprocess` (`reader.c:4267`)
  receives a buffer whose coordinates are the original file's, so its own
  `SweetMap` stays correct without any awareness that an envelope ran.
- **The reader needs no changes.** It sees whitespace between forms, which it
  already skips.
- The pass is a line-classifying scan with a small state machine (in-fence /
  out-of-fence) and no recursive grammar.

Cost: the intermediate buffer is as large as the Markdown file, most of it
spaces. For source files this is irrelevant.

---

## Part 1 -- Graduate the `stringed` layer

### Motivation

`stringed` is the only live `#lang` layer, and it registers exactly one
dispatch: `#s"` => `(string/from-cstr $body)` (`lang_layers.c:39-57`). Every
other `#`-dispatch in the language (`#map{}`, `#set{}`, `#rat{}`, `#refine{}`,
`#r{}`, `#json()`, `#fx{}`, `#reads`, `#writes`, `#rx""`) is always-on.
CLAUDE.md says to graduate a layer to always-on rather than let layers
accumulate, and `stringed` has no ordering dependence, no semantic gate, and
no cost when unconditional. It is the textbook graduation.

After graduation the only ceremony for owned Strings is
`(load "stdlib/string.tur")`.

### The read/eval split (why `(load)` stays)

A reader macro is installed at **read** time; `(load)` runs at **eval** time.
Graduating makes the read-time half unconditional -- `#s"hi"` always *reads*
as `(string/from-cstr "hi")` -- but evaluating that form still needs
`string/from-cstr` (`stdlib/string.tur:112`) to be defined. That is precisely
the "only thing needed is to load the library" end state.

Adding `string.tur` to the autoload list (`stdlib_autoload.c:18`) would remove
even that, but it pulls a refcounted-heap type into every compilation unit
whether or not the program uses Strings, and changes the ABI surface of every
emitted TU. **Out of scope here** -- a separate decision.

### Steps

**S1 -- Register `#s"` unconditionally.** Move the body of
`stringed_reader_hook` (`lang_layers.c:39`) into the reader init path, called
from `read_all_with_registry_from` right after `reg` is established and next
to the existing `lang_layers_apply_readers` call (`reader.c:4413`). Suggested
home: a `reader_macros_install_builtins(reg, arena, st)` in `reader_macros.c`,
so the next graduated dispatch has an obvious place to land rather than
accreting one-offs in `reader.c`.

The existing hook is already idempotent -- it early-returns on
`reader_macros_lookup(reg, "s", '"')` -- which is what keeps the persistent
REPL/interp registry (`env.h:364`, `env.c:612`) safe across repeated calls.
Keep that guard; it is now load-bearing for a different reason (see S4).

**S2 -- Delete the `LANG_LAYERS[]` row** (`lang_layers.c:62-68`). The table
becomes empty.

> **Gotcha: an empty array initializer is not standard C.**
> `static const T LANG_LAYERS[] = {};` is a GNU/Clang extension, and
> `lang_layers_count()` (`lang_layers.c:140`) computes
> `sizeof(LANG_LAYERS)/sizeof(LANG_LAYERS[0])` on it. Fix it the way
> `GRADUATED_LAYERS[]` already does: keep a `{ NULL, ... }` sentinel row and
> count by walking to the first `NULL` name. Then `lang_layer_at`,
> `lang_layer_index`, and both iteration loops (`lang_layers_apply_readers`,
> `lang_layers_apply_semantic`) work unchanged against a count of 0. Do this
> in the same commit as the row deletion, not as a follow-up.

**S3 -- Add `"stringed"` to `GRADUATED_LAYERS[]`** (`lang_layers.c:115`) so
`#lang turmeric stringed` is an accepted no-op with a one-time TUR-W0064
instead of a hard TUR-E0330. This is exactly the path `refined` took; the
table's own comment explains it exists for this moment. Age the entry out one
minor line later, at which point the token becomes the same hard error any
unknown token gets.

Note the pairing rule in that comment does **not** apply here: it binds a
*semantic* layer to its `EXPERIMENTS[]` row. `stringed` is a reader layer with
no experiment, so there is no `GRADUATED[]` counterpart in `experiments.c` to
retire alongside it.

**S4 -- Retire `stdlib/string-reader.tur`.** This is the one step with a
sharp edge the superseded draft missed. Once `#s"` is registered
unconditionally, `#use-reader-macros "stdlib/string-reader.tur"` re-registers
the same `(name, delim)` pair against a **strict** batch-compile registry,
and `reader_macros_register` makes that a hard error with a "previously
registered here" note (`reader_macros.c:157`). So the file cannot simply be
left alone -- it would turn from working code into a compile error.

Recommended: **gut the file to a comment-only no-op for one minor line, then
delete it**, matching the `GRADUATED_LAYERS[]` migration-window philosophy.
Replace line 28 (`(reader-macros/define 's :string ...)`) with a `;;;` note
saying the macro is now always-on and the directive is unnecessary. A file in
the wild naming it keeps compiling; a reader of the file learns why.

Alternative (blunter): delete it now and let callers get "file not found".
Rejected as the default because it breaks working files at graduation, which
is the exact failure mode the layer shim exists to prevent.

**S5 -- Fixtures.**

- `tests/fixtures/string-reader-macro/input.tur` -- **must change in this
  PR**, per S4: line 3 is `#use-reader-macros "stdlib/string-reader.tur"`,
  which becomes a hard error. Drop the directive; the fixture then pins the
  post-graduation idiom (`(load)` + `#s"..."` and nothing else). Its Map-key
  and `eq?` assertions are worth keeping -- they are the broadest `#s`
  coverage in the suite.
- `tests/fixtures/lang-layer-stringed/input.tur`,
  `lang-layer-stringed-sweet/input.tur` -- drop `stringed` from the `#lang`
  line in one and keep it in the other, so the suite pins both the clean
  idiom and the TUR-W0064 shim. Note the sweet fixture is the only thing
  pinning "`#s"` works under a non-default base," so keep that coverage
  wherever it ends up.
- `tests/fixtures/lang-trailing-tokens/input.tur` -- its whole point is that
  trailing tokens are stripped and never leak into the body. It uses
  `stringed` as the token; after graduation that token is a *graduated*
  no-op, which exercises a different code path. Swap in a live layer... except
  there are none left. Either keep `stringed` and accept that it now tests
  the graduated path, or point it at a deliberately unknown token and pair it
  with the TUR-E0330 expectation. Lean: keep it, and add a comment that the
  stripping behavior is what is pinned, not the token's status.
- `tests/fixtures/errors/lang-layer-retired-name/` -- pins TUR-E0330 for
  `refined`. Add a `stringed` case only in the age-out PR, not this one.

**S6 -- Listings and the playground.**

- `tests/wasm_glue_lang_unit.c:90` asserts the registry JSON contains
  `"name":"stringed"`. **This will fail** once the row is gone; update it to
  assert an empty `layers` array. Line 65-69 (`set_lang("turmeric stringed")`
  activates `#s"`) still passes, but now for a different reason -- retarget
  it at the graduated-token path or delete it.
- `cmd_lang_layers` (`main.c:10524`) already handles `n == 0` on both the
  human and `--json` paths. No change needed; verify the empty output reads
  sensibly.
- `turi_wasm_lang_registry` (`wasm_glue.c:505`) walks the table live, so it
  emits `"layers":[]` with no change. Check `web/main.js:5546` and the lang
  picker render an empty layer list gracefully rather than showing an empty
  control.

**S7 -- Docs.**

- `docs/guides/syntax-guide.md:537-556` -- "The reader layer available today
  is **`stringed`**" becomes "no reader layer today"; move `#s"..."` into the
  always-on dispatch table. Also `syntax-guide.md:20`, which uses
  `#lang turmeric stringed` as the example of a layer token.
- `docs/guides/reader-forms-guide.md` -- the "Owned-String literal --
  `#s"..."` (layer `stringed`)" heading loses its parenthetical. Note this
  heading is an anchor target in the generated site
  (`#owned-string-literal-s-layer-stringed`), so the docs rebuild is part of
  the change.
- `docs/guides/strings-guide.md`, `web/README.md:151`, `CHANGELOG.md`.
- Per `feedback_guides_link_plans_via_github_url`: any guide reference to this
  plan is a GitHub URL, not a relative path.

### Risk

Low. The expansion target is unchanged; the only behavioral change is that
`#s"..."` works without a token. No collision: the built-in `#s(` set literal
is a different delimiter, and `reader_macros_is_reserved` (`reader_macros.c:139`)
already lets `("s", '"')` through today.

The one non-obvious failure is S4 -- a `#use-reader-macros` of the old file
going from working to hard error -- and S6's wasm unit test, which fails
loudly. Both are listed above precisely so they land in the same PR.

---

## Part 2 -- The envelope axis and literate `.tur.md`

### What a literate `.tur.md` is

A Markdown file that *is* a compilation unit. Prose is prose; fenced blocks
tagged with a Turmeric base dialect are the program.

````markdown
# Squares

This module computes squares.  Everything outside a tagged fence is prose and
is invisible to the compiler.

```turmeric
(defn square [x : int] : int
  (* x x))
```

Multiple blocks concatenate in file order, so a later block sees the earlier
one's definitions:

```turmeric
(defn main [] : int
  (println (square 7))
  0)
```
````

Same file with sweet-exp, changing nothing but the fence tag:

````markdown
```turmeric/sweet
defn square [x : int] : int
  *(x x)
```
````

### Why this and not the notebook spice

The notebook spice (`turmeric-spices/spices/notebook`) already reads `.tur.md`
and evaluates its cells -- it has cell extraction (`src/notebook/cell.tur`),
fence-attribute parsing, session-backed eval, caching, and HTML/TUI rendering.
It uses the same ```` ```turmeric ```` fence tag, including attribute form
(```` ```turmeric {math=true} ````).

What it does **not** give is `.tur.md` as a *compilation unit*: `tur build`,
`tur check`, `(import)` of a `.tur.md` module, fixtures, `emit-c`, LSP. A
notebook is a document you run cell-by-cell in a session; a literate module is
a program you compile. This plan is the second thing, and it should share the
first thing's surface syntax so one file can be both -- open a literate module
in `nb tui` and it just works.

**Coordinate the fence grammar with the spice.** Anything this plan adds
(recognized tags, an attribute meaning "not part of the program") must be
something the notebook's parser already tolerates or is changed to tolerate,
in the same window. Two `.tur.md` dialects that disagree about which fences
are code would be worse than either alone.

### Why fence tags, not `#lang`

Base dialect selection inside a `.tur.md` comes from the **fence info
string**, not a `#lang` line:

- `#lang turmeric/sweet` at the top of a Markdown file renders as an `<h1>`
  reading "lang turmeric/sweet". The whole point of the format is that it
  renders.
- The fence tag is where a Markdown author already declares a language, it is
  what every renderer and editor uses for highlighting, and it is what the
  notebook spice already parses.
- `detect_lang_layered` (`reader.c:4551`) requires `#lang` on the first line
  of the buffer, which a Markdown file's first line is not.

MVP: **one base per file.** The first tagged fence sets it; a later fence
with a different tag is a hard error (`TUR-E03xx`, "conflicting base dialect
in literate file"). Per-block dialects are deferred -- see Open questions.

### Which fences are code

- ```` ```turmeric ````, ```` ```turmeric/sweet ````, ```` ```turmeric/neoteric ````,
  ```` ```turmeric/curly-infix ```` -- code, in the named base. Reuse
  `lang_base_from_name` (`reader.c:4532`) for the tag, so the recognized set
  cannot drift from `#lang`'s. That also means `sweet-exp` (the legacy alias)
  is accepted, and a future `turmeric/at-exp` works with no extra wiring.
- Any other tag (` ```c `, ` ```sh `, ` ```json `, untagged) -- prose.
  Deliberately: a literate module shows shell commands and C output as
  illustration all the time.
- **Indented (4-space) Markdown code blocks -- prose.** They carry no language
  tag, so treating them as code would silently compile examples.
- Tags carrying attributes (```` ```turmeric {id=setup} ````) -- code; the
  attributes are for the notebook and are ignored here. One attribute is worth
  reserving from day one: an opt-out (`{example}` / `{no-compile}`) for a
  Turmeric block that is *illustration*, not part of the program. Without it,
  a literate doc cannot show a snippet of bad code. Confirm the spelling with
  the notebook spice before shipping.

The scanner also has to respect Markdown's real fence rules or it will
mis-slice files: a fence may be ```` ``` ```` **or** `~~~`, may use more than
three characters, and is closed only by a run of the same character at least
as long. Blocks inside blockquotes and list items are indented; MVP can
require code fences at column 0 and diagnose an indented one, but must not
silently misread it.

### Implementation

**E1 -- The envelope concept.** New `SourceEnvelope` enum in `diag.h` next to
`ReaderType` (`diag.h:388`):

```c
typedef enum SourceEnvelope {
    ENVELOPE_NONE = 0,     /* plain source; the only value before this plan */
    ENVELOPE_LITERATE_MD,  /* .tur.md -- fenced Turmeric inside Markdown */
} SourceEnvelope;
```

Add a `SourceEnvelope envelope;` field to `SourceFile` (`diag.h:437`),
defaulting to `ENVELOPE_NONE`. Zero-initialization makes every existing
construction site correct without edits.

**E2 -- Extension dispatch.** `reader_type_from_extension` (`reader.c:4665`)
currently answers one question with one return value. Split it:
`source_format_from_extension(path, &envelope, &base)` returning both, with
`reader_type_from_extension` kept as a thin wrapper so the ~9 existing call
sites (`main.c:176`, `main.c:7355`, `main.c:7615`, `eval.c:12414`,
`wasm_lsp.c:146`, `lsp.c:2111`, ...) compile unchanged and are converted one
at a time.

For `.tur.md`: envelope = `ENVELOPE_LITERATE_MD`, base = *unresolved* (it
comes from the first fence, which needs the buffer). Have the envelope pass
report the base it found, and set `file->reader_type` from that before the
reader branches on it.

**E3 -- The pass.** `literate_md_blank(arena, src, len, &out_base)` returning
a same-length buffer:

- Scan line by line with an in-fence / out-of-fence state machine.
- Out of fence: blank every byte except `\n` (and `\r`).
- On an opening fence, classify the info string. If it names a base, record
  it (first one wins; a conflicting later one is the E2 error) and enter
  code mode; otherwise enter prose-fence mode.
- In code mode: copy bytes verbatim.
- In prose-fence mode: blank as prose.
- The fence lines themselves are always blanked -- including the opening one,
  so the info string never reaches the reader.
- If no tagged fence exists, the output is entirely blank. That reads as an
  empty program, which is right for a `.tur.md` that is pure prose, but a
  file the user asked to *build* with zero code blocks deserves a warning.

**E4 -- Wire into the reader.** In `read_all_with_registry_from`
(`reader.c:4347`), run the envelope **before** the `READER_SWEET` branch at
`reader.c:4357`:

```
if (file->envelope == ENVELOPE_LITERATE_MD) { ... }   /* new, first */
if (eff_file->reader_type == READER_SWEET) { ... }    /* existing, on the result */
```

Because the pass is length- and newline-preserving, the envelope's
`SourceFile` copy needs **no** `orig_src`/`xform_map` -- spans are already in
original coordinates. Register the blanked buffer as the file contents so the
reader's `r.src` and the diag registry agree, exactly as the sweet branch
does; diagnostics rendering a source line will print the blanked line, so if
that turns out to matter, keep the original as `orig_src` with a null map and
teach the renderer to prefer it. Verify which reads better before deciding.
A `TUR_LITERATE_DUMP=1` env var mirroring `TUR_SWEET_DUMP` is worth having.

**E5 -- Interpreter path.** `eval.c:12103` guards incremental re-read on
`reader_type != READER_SWEET`, because a whole-buffer transform makes a
non-zero start offset meaningless. The blanking pass does **not** have that
problem -- offsets are preserved -- so literate files could in principle stay
incremental. Do not exploit that in the MVP: add
`envelope == ENVELOPE_NONE` to the same guard, get it correct, and revisit.
`eval.c:12414` and `eval.c:12417` are where the extension sets
`env->reader_type` and will need the envelope alongside.

**E6 -- The rest of the toolchain.** Every place that decides "is this a
Turmeric source file" by extension needs `.tur.md`:
`main.c:7325` (`tur fmt`), the directory walk at `main.c:7427`, the
`tur run` single-file dispatch at `main.c:11892`, project-mode build descent
in `pkg.c`, and module resolution for `(import)`. `tur fmt` on a `.tur.md`
is genuinely hard (reformat inside fences, leave prose alone) -- **skip it in
the MVP and diagnose rather than mangle**.

### Phasing

- **L0** -- `SourceEnvelope`, the `SourceFile` field, extension dispatch,
  passthrough pass. `.tur.md` is recognized and compiles as if empty.
- **L1** -- The blanking pass with ```` ```turmeric ```` fences only, plain
  base. `tur run foo.tur.md` and `tur build foo.tur.md` work. Fixtures.
  **This is the MVP and the point at which the axis has paid for itself.**
- **L2** -- Base selection from the fence tag via `lang_base_from_name`;
  `turmeric/sweet` literate files. This is the step the whole design exists
  for, so do not let L1 ship alone for long.
- **L3** -- Full Markdown fence grammar (`~~~`, longer runs, indented fences
  diagnosed), the `{example}` opt-out attribute, conflicting-tag error.
- **L4** -- Toolchain surface: `(import)` of a `.tur.md` module, project-mode
  descent, LSP, `tur check`.
- **L5** -- Docs (`syntax-guide.md`, a literate guide), notebook-spice
  coordination, `tur lang-envelopes` listing if the axis grows a second
  member.

### Open questions

- **Per-block base dialects.** Attractive (a doc could show the same function
  in both syntaxes) but it breaks the cheap composition: `sweet_preprocess`
  runs on the whole buffer, so mixed bases would need per-block preprocessing
  and offset bookkeeping the blanking trick currently avoids. Deferred, and
  the "first tag wins, conflicts error" MVP rule keeps the door open --
  relaxing an error later is compatible; tightening is not.
- **Does the envelope axis need a registry?** With one member, a `switch` is
  honest and a `LANG_ENVELOPES[]` table would be ceremony. Add the table when
  a second envelope appears, not before. If one never appears, that is a
  finding: the axis was really just "`.tur.md` support," and it should be
  said plainly rather than dressed up.
- **Ordering within a file.** MVP concatenates blocks in file order. Racket's
  `scribble/lp` supports out-of-order `chunk` weaving. Explicitly out of
  scope; if it is ever wanted it is a separate feature on top.
- **`#lang` inside a fence.** Should ```` ```turmeric ```` + a `#lang` line as
  the fence's first line be honored? It would give per-block layers. It also
  gives two ways to say the same thing. Lean: error in the MVP, revisit with
  per-block bases.
- **Empty-program warning.** A `.tur.md` with no tagged fence compiles to
  nothing. Warn on an explicit build of such a file; stay silent when it is
  merely walked.

### Risk

Low for L1-L2 and mostly bounded by the Markdown scanner. The blanking design
removes the two things that would have made it risky -- offset mapping and
composition with `sweet_preprocess`. The real exposure is scope creep across
E6: every extension check in the tree is a place `.tur.md` can be half-wired,
producing "works with `tur run`, invisible to `tur build .`" confusion. Land
E6 deliberately, one surface at a time, and say in the docs which surfaces are
live.

---

## Part 3 -- `turmeric/at-exp` base dialect (carried forward)

Retained from the superseded draft. Unchanged in substance: at-exp **is** a
base dialect, and it is the clean contrast that makes Part 2's axis argument
concrete.

### What it is

A port of Racket's `at-exp` reader: `@cmd[datum...]{body...}` reads as
`(cmd datum... "body"...)`. Spec:
https://docs.racket-lang.org/scribble/reader.html

| Input | Reads as |
|---|---|
| `@foo{blah blah}` | `(foo "blah blah")` |
| `@foo[1 2]{3 4}` | `(foo 1 2 "3 4")` |
| `@foo[1 2 3]` | `(foo 1 2 3)` |
| `@foo{bar @baz{3} blah}` | `(foo "bar " (baz "3") " blah")` |
| `@{blah blah}` | `("blah blah")` |
| `@foo` | `foo` |
| `@;{...}` / `@;line` | comment |

### Why it IS a base dialect

It takes over `@`, which is currently the deref prefix (`read_at`,
`reader.c:3412`), and it changes how `{`/`}` read inside a body (balanced
braces are literal text, not curly-infix). Those are whole-reader semantics,
not an additive `#`-dispatch and not a wrapper around another reader. A file
opts in knowing deref must be written `(deref x)`.

Contrast with literate mode, which takes over nothing: it *delimits* regions
and hands each to an unmodified base reader. That is the test for which axis
a feature belongs on -- **does it change how the base reads a byte, or does it
decide which bytes the base sees?**

### Implementation sketch

Follow the `sweet_preprocess` precedent: `READER_AT_EXP` in the `ReaderType`
enum, a row in `lang_base_from_name` (`reader.c:4532`), entries in
`reader_type_name` (`reader.c:4678`) and `reader_type_is_implemented`
(`reader.c:4690`), and an `at_exp_preprocess` producing s-expression source
plus a `SweetMap`-style offset map (this one *does* need a map -- it is not
length-preserving). A third branch alongside `READER_SWEET` in
`read_all_with_registry_from`, and the same `eval.c:12103` guard.

Because at-exp is a base, it composes with the Part 2 envelope for free: a
```` ```turmeric/at-exp ```` fence inside a `.tur.md` needs no new wiring.

### Phasing

- **A0** -- Enum, name recognition, passthrough preprocessor.
- **A1** -- `@cmd{body}` and `@cmd[datum]{body}`, literal bodies, no nesting;
  `@;` line comment. Covers templating, the common case.
- **A2** -- Nested `@` forms, `@"string"` merging, `@|expr|` escapes,
  `@;{...}` block comments.
- **A3** -- Newline and indentation rules (hardest; needs line/column
  accounting).
- **A4** -- Alternative body delimiters (`|{...}|`, `|--{...}--|`). Defer
  until there is a user need.
- **A5** -- Docs and a fixture per row of the spec table.

---

## Dependency order

Part 1 and Part 2 are fully independent -- different files, different axes --
and either can land first. Part 1 is much smaller and empties
`LANG_LAYERS[]`, which is a tidy state to be in before touching reader
plumbing, so it is the natural opener. Part 3 depends on neither, and Part 2's
fence dispatch picks it up automatically if it lands later.

None of the three needs an `--enable` experiment. Per CLAUDE.md, the
experiment gate covers elaboration/checker gates and codegen knobs; reader
selection is `#lang`/extension, and a graduation is the removal of a gate,
not the addition of one.

## Summary

| Piece | Axis | Selected by | Composes with | Needs an offset map? |
|---|---|---|---|---|
| `stringed` graduation | (removes a layer) | -- always on | everything | n/a |
| Literate `.tur.md` | **envelope** (new) | `.tur.md` extension | every base | **No** -- blanking preserves offsets |
| at-exp | base dialect | `#lang turmeric/at-exp` | layers, envelopes | Yes |

## Out of scope

- Autoloading `stdlib/string.tur` (zero-ceremony owned Strings) -- a separate
  autoload-list decision with real ABI cost.
- Per-block base dialects in a `.tur.md`.
- Chunk weaving / out-of-order code assembly (`scribble/lp`).
- Rendering literate prose to documentation -- that is the notebook spice's
  job, and the shared fence grammar is what connects the two.
- `tur fmt` on `.tur.md`.
- Alternative at-exp body delimiters.
