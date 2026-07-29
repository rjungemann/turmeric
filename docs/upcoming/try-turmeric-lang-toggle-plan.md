# Try Turmeric: visual `#lang` picker

> **Status:** proposed (2026-07-29)
> **Type:** Web playground / `#lang` plumbing
> **Related:** [`lang-layers-plan.md`](lang-layers-plan.md),
> [`try-turmeric-lsp-plan.md`](try-turmeric-lsp-plan.md)

## 0. Summary

A user landing on Try Turmeric has no way to discover that the language has
four reader dialects and a curated layer set. The only signal is the `#lang`
line in whatever example loaded, and the only way to change dialect is to know
the exact spelling and type it. Give the editor header a **dialect picker plus
layer toggles** that edit the `#lang` line in place -- so the control teaches
the syntax rather than hiding it.

**The `#lang` line stays the source of truth.** The picker is a text edit, not
a hidden mode. Everything in the buffer round-trips: paste a file with a
`#lang` header and the picker updates; flip the picker and the header updates.
Nothing is stored in UI state that is not also in the source.

Two real plumbing gaps surfaced while designing this (§2); both need fixing
before the UI is honest.

---

## 1. What exists

### 1.1 The `#lang` grammar

`detect_lang_layered` (`src/compiler/reader.c:4135`) parses
`#lang <base> <layer>*`. Bases (`lang_base_from_name`,
`src/compiler/reader.c:4116`):

| Directive | ReaderType |
|---|---|
| `#lang turmeric` | `READER_TURMERIC` |
| `#lang turmeric/curly-infix` | `READER_CURLY_INFIX` |
| `#lang turmeric/neoteric` | `READER_NEOTERIC` |
| `#lang turmeric/sweet` | `READER_SWEET` |
| `#lang sweet-exp` | `READER_SWEET` (legacy alias, accepted through v1) |

Layers (`LANG_LAYERS[]`, `src/compiler/lang_layers.c:63`) -- exactly two today:

| Layer | Kind | Effect |
|---|---|---|
| `stringed` | reader | `#s"..."` reads as `(string/from-cstr "...")` |
| `refined` | semantic | static discharge of `#refine{...}`; gated on the `refined` experiment |

`GRADUATED_LAYERS[]` (`lang_layers.c:113`) is deliberately empty.

### 1.2 The playground today

- `parseLangDirective()` (`web/main.js:894`) regex-matches the first line and
  returns `{lang, body}` -- **base token only; trailing layer tokens are
  discarded.**
- `currentLangMode` (`main.js:30`) tracks the base for a UI indicator.
- Each eval forwards `lang` to the worker as a hint (`main.js:940`).
- `turi_wasm_set_lang(name)` (`src/web/wasm_glue.c:398`) synthesizes
  `"#lang <name>"`, runs `detect_lang` (base-only), and on a base change calls
  `turi_env_reset_to_prelude` before swapping `env->reader_type`.
- The Examples menu (`web/try/index.html:88-96`) has a "Sweet-exp Syntax"
  entry, which is the only discovery path that exists.

---

## 2. Two gaps that have to be fixed first

These are not UI polish; a picker built on top of the current plumbing would
be lying to the user.

### 2.1 `turi_wasm_set_lang` cannot express layers

It calls `detect_lang` (`wasm_glue.c:408`), the base-only wrapper, which
*parses and discards* layer tokens (`reader.c:4242-4247`). Toggling
`stringed` in a picker would therefore change the text and change nothing
else. Fix: switch to `detect_lang_layered`, thread the `LangLayerSet` onto
`g_env->lang_layers`, and reject an unknown token via the `out_bad` out-param
instead of silently ignoring it.

### 2.2 Layers accumulate and never clear in the interpreter env

`src/turi/eval.c:10187` does `env->lang_layers |= layers;`. Within a REPL
session that is deliberate for `#lang` lines arriving turn by turn, but it
means **a layer can be turned on and never off**. A picker whose checkbox
does not uncheck is worse than no picker.

Fix: `turi_wasm_set_lang` *assigns* rather than ORs, and treats a layer-set
change the same way it treats a base change -- `turi_env_reset_to_prelude`,
then set. Losing session state on a language switch is already the accepted
behaviour for the base (`wasm_glue.c:413-421`); extending it to layers is
consistent, and the UI warns before discarding (§3.4).

### 2.3 A naming round-trip mismatch (smaller, but user-visible)

`reader_type_name(READER_SWEET)` returns `"sweet-exp"`
(`src/compiler/reader.c:4265`), the legacy alias -- so `turi_wasm_get_lang()`
reports `sweet-exp` for a buffer whose header says `turmeric/sweet`. A picker
that reads back its own state via `get_lang` would flicker between two
spellings for one selection.

Fix: `reader_type_name` returns the canonical slash-namespaced
`"turmeric/sweet"`. `lang_base_from_name` keeps accepting `sweet-exp` on
input, so no file breaks. Audit callers of `reader_type_name` first -- it
appears in diagnostics and REPL output, and those strings may be asserted on
in fixtures; regenerate what moves, in the same change.

---

## 3. Design

### 3.1 Registry export, so the UI is never a second source of truth

CLAUDE.md's anti-proliferation rule says `LANG_LAYERS[]` is the single source
of truth. A hardcoded JS list would quietly become a second one and drift on
the next layer added or graduated.

Add to `wasm_glue.c`:

```c
/* JSON: {"bases":[{"name":"turmeric","label":"S-expression"},...],
 *        "layers":[{"name":"stringed","kind":"reader",
 *                   "summary":"#s\"...\" owned-String literal","since":"v1",
 *                   "available":true}]} */
const char *turi_wasm_lang_registry(void);
```

Bases come from `lang_base_from_name`'s accepted set (the legacy `sweet-exp`
alias is *not* offered -- accepted on input, never generated). Layers are
walked from `LANG_LAYERS[]` via the existing `lang_layers_count()` /
descriptor accessors that back `tur lang-layers` (`src/main.c:74`). The
`available` flag is false for a semantic layer whose experiment is disabled or
expired, which keeps §3.3 honest.

Human-readable base labels ("S-expression", "Curly-infix", "Neoteric",
"Sweet-expression") live in the C table next to the names, not in JS.

### 3.2 The control

In `web/try/index.html`, in `.editor-actions` before the Examples button: a
button showing the current dialect (`sweet`, `s-expr`, ...) opening a popover
that reuses the existing `.more-menu` styling -- no new popover machinery.

```
  Dialect            Layers
  ( ) S-expression   [ ] stringed   #s"..." string literals
  ( ) Curly-infix    [x] refined    static #refine{...} checking  (experimental)
  ( ) Neoteric
  (o) Sweet-expression
```

Radio group for the base (mutually exclusive, per the `#lang` grammar);
checkboxes for layers (an order-independent set, per the same grammar). The
form of the control mirrors the form of the syntax -- that is the point.

On mobile the control collapses into the existing `#more-menu` overflow
(`index.html:110-125`) as a "Language..." item, alongside Clear/Format/Share.

### 3.3 Experimental layers are labelled as such

`refined` is a semantic layer pointing at an `EXPERIMENTS[]` row
(`lang_layers.c:76-80`). It renders with an "experimental" chip and its
summary as tooltip text. If `available` is false, the row is shown disabled
with the reason -- never hidden, because hiding it makes the experiment
undiscoverable and makes the picker disagree with `tur lang-layers`.

### 3.4 Editing the source

One function, `setLangDirective(model, {base, layers})`, is the only writer:

- **No `#lang` line, and the selection is the default** (`turmeric`, no
  layers) -- write nothing. Do not decorate a plain file with a redundant
  header.
- **No `#lang` line, non-default selection** -- insert `#lang <base> <layers>`
  as line 1, followed by a blank line.
- **Has a `#lang` line** -- replace exactly that line, preserving everything
  after it. Layers are emitted in `LANG_LAYERS[]` registry order so the text
  is stable across toggles (the set is order-independent to the reader, but a
  jittering line makes a noisy diff and a noisy undo stack).
- **Selection returns to the default** -- remove the line, and the blank line
  after it if this insert added one.

All of it goes through a single `model.pushEditOperations`, so **one Ctrl+Z
undoes a language switch.**

Round-trip the other way: on model content change (already debounced for the
LSP), re-parse line 1 and reconcile the picker. Typing the header by hand and
using the picker are the same operation.

`parseLangDirective` (`main.js:894`) is extended to return
`{base, layers[], line}` -- it currently drops the trailing tokens, mirroring
gap §2.1 on the JS side.

### 3.5 Per-tab, not global

`currentLangMode` is a module-level global (`main.js:30`), but tabs are
independent files and Turmeric's dialect is per-file. The picker reflects the
**active tab**, and switching tabs re-reads that tab's line 1. Nothing about
the dialect is persisted outside the tab's own text.

### 3.6 Interaction with running code

Flipping the base or layer set resets the environment to prelude (§2.2).
The user is told before it happens -- a one-line confirm in the popover
footer ("Switching resets the REPL environment") rather than a modal, matching
how the Clear button's shift-click reset already behaves.

### 3.7 Editor-side follow-through (secondary, but why bother otherwise)

- **Monaco brackets.** Sweet-exp and neoteric change what a well-formed line
  looks like; the language configuration (`main.js:1017`) is one shared
  `turmeric` config. Registering per-dialect configuration is possible but is
  a bigger job -- **out of scope here**, recorded so it is not mistaken for an
  oversight.
- **Examples.** The "Sweet-exp Syntax" example entry stays; the picker is a
  second, more discoverable path to the same thing.
- **Default document.** `DEFAULT_CODE` already opens with
  `#lang turmeric/sweet` (`main.js:37`), so a first-time user sees the picker
  agreeing with the buffer immediately.

---

## 4. Phases

- **T0 -- plumbing.** §2.1, §2.2, §2.3. C-side only; testable through
  `tur`/`turi` with no web change. `reader_type_name` is the one with fixture
  fallout -- regenerate in the same commit, per the fixture-churn rule.
- **T1 -- registry export.** `turi_wasm_lang_registry`, added to
  `-sEXPORTED_FUNCTIONS` (`src/CMakeLists.txt:1222`).
- **T2 -- picker UI.** §3.2, §3.4, §3.5. Base radio group first; the layer
  checkboxes only after T0 makes them do something.
- **T3 -- polish.** Experimental chips, mobile overflow placement, the
  reset warning, keyboard navigation of the popover.

---

## 5. Testing

- **C-side:** fixtures for `#lang turmeric/sweet stringed` covering layers-on
  then layers-off in one interpreter session (the §2.2 regression), and a
  `reader_type_name` round-trip assertion.
- **Playwright** (`web/tests`): pick sweet-exp on an empty buffer -> header
  appears; pick S-expression -> header disappears; type a header by hand ->
  picker reconciles; Ctrl+Z after a switch -> exactly one undo restores the
  previous text; switch tabs -> picker follows the tab.
- Twelve-minute timeout on every suite run.

---

## 6. Explicitly not doing

- Inventing new `#lang` bases or layers for the UI's benefit. The picker
  offers what `LANG_LAYERS[]` and `lang_base_from_name` already offer -- an
  additional layer is a compiler decision made under CLAUDE.md's curation
  rule, never a playground decision.
- Offering the legacy `sweet-exp` spelling as a choice.
- A "convert my file between dialects" button. Translating s-expressions to
  sweet-exp is a formatter feature, not a picker feature.
