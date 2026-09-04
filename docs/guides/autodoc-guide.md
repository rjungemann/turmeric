---
title: Autodoc Guide
category: CLI Tools
description: Writing `;;;` docstrings and generating API docs with `tools/gendocs.py`
---

# Autodoc Guide

Turmeric's documentation system has three layers:

1. **`;;;` docstrings** -- triple-semicolon comments above any `defn`, `defmacro`,
   `defstruct`, `definstance`, or `defopaque`.
2. **HTML API docs** -- `tools/gendocs.py` parses the docstrings and emits a
   styled site under `docs/html/api/`.
3. **Runtime `(doc name)`** -- looks up the docstring for a name at the REPL or
   in the web playground. The same generator can emit `stdlib/docstrings.tur`,
   the lookup table the `doc` macro reads.

This guide covers all three.

---

## Writing docstrings

Use `;;;` (triple-semicolon), distinct from `;;` (regular inline comment).
A docstring block must sit **immediately above** the definition it documents --
a blank line or any non-`;;;` line resets the buffer.

```turmeric
;;; cons -- prepend a value to a list.
;;;
;;; Parameters:
;;;   value -- the element to prepend
;;;   next  -- the existing list (or nil-value for empty)
;;;
;;; Returns:
;;;   A new Cons cell pointing to next.
;;;
;;; Example:
;;;   (cons 1 (cons 2 (nil-value)))  ; => (1 2)
;;;
;;; Since: Phase B1
(defn cons [value next] : int
  ...)
```

### Required fields

| Field | Required | Notes |
|---|---|---|
| One-line summary (first `;;;` line) | Yes | `;;; name -- brief description` |
| `Parameters:` block | If non-zero arity | One `;;;   name -- desc` line per param |
| `Returns:` | Yes, unless `:void` | Describe the return value |
| `Example:` | Yes | At least one usage example with `; => result` |
| `Since:` | When known | Phase tag, e.g. `Phase B1` |

### Conventions

- First line: `;;; name -- brief summary` -- the name is repeated so the line is
  greppable.
- Blank `;;;` lines separate sections, like Python docstrings.
- Examples use `; => result` to show expected output.
- **ASCII only** -- use `--` (double hyphen), never em dashes. The Turmeric
  parser hangs on non-ASCII bytes.
- Internal helpers (e.g. `tur-contract-check`, `__functor_*`) get a one-liner
  only -- no Parameters/Returns/Example blocks needed.

### Docstring levels

| Audience | Format |
|---|---|
| Exported / public API | Full block (summary + Parameters + Returns + Example + Since) |
| Internal helpers | Single-line `;;; name -- what it does` |
| Typeclass instances (`definstance`) | Single-line summary |

### Module docstrings

A contiguous `;;;` block at the very top of a file -- **before the first real
definition** -- becomes the module docstring. Terminate it with a `;;` line or
any non-comment, non-blank form (e.g. `(defmodule ...)`, `(export ...)`).

```turmeric
;;; tur/list -- untyped singly-linked Cons/nil list.
;;;
;;; Legacy list implementation; prefer tur/list for new code.
;;;
;;; Since: Phase B1
;; List type for Turmeric                  <-- terminates the module block
(defstruct Cons ...)
```

The generator renders the module block as the description paragraph on the
per-module HTML page and registers the module name in the runtime lookup
table, so `(doc 'tur/list)` returns the summary.

---

## Generating docs

The `tur run docs` recipe (or `python3 tools/gendocs.py` directly) regenerates
the HTML site and the runtime lookup table.

```sh
tur run docs

# Equivalent direct invocation:
python3 tools/gendocs.py stdlib/ \
    --out docs/html/api/ \
    --emit-tur stdlib/docstrings.tur
```

The `tur run docs` recipe additionally emits `web/public/doc-names.json`
(the web REPL search index) via `--emit-json`, folding in spice symbols
with `--extra-json`.

**Generated artifacts live under `docs/html/api/` -- do not edit by hand.**
Each regen overwrites the tree.

### Output layout

```
docs/html/api/
  index.html          -- module index with one-liners
  tur-list.html       -- per-module page
  tur-option.html
  ...
  style.css           -- shared stylesheet
```

Each per-module page has:

- The module docstring as a header paragraph.
- A sidebar TOC of exported names with anchor links.
- One card per exported definition: kind badge (`defn` / `defmacro` /
  `defstruct` / `definstance`), signature, summary, Parameters, Returns,
  Example, Since.
- A collapsed "Internal" section for non-exported definitions.

### Single-module regen

When iterating on docstrings in a single file:

```sh
python3 tools/gendocs.py stdlib/list.tur --out docs/html/api/
```

---

## Runtime `(doc name)`

`stdlib/docstrings.tur` is **auto-generated** by `gendocs.py --emit-tur`. It
defines the `tur/docstrings` module, whose `doc-lookup` function maps a name
to its docstring via a static table. The `doc` macro in
`stdlib/macros.tur` reads it:

```
tur> (doc cons)
cons -- prepend a value to a list.

Parameters:
  value  the element to prepend
  next   the existing list (or nil-value for empty)

Returns:
  A new Cons cell pointing to next.

Example:
  (cons 1 (cons 2 (nil-value)))  ; => (1 2)

Since: Phase B1
```

`(doc name)` accepts either a bare symbol (`(doc cons)`) or a string
(`(doc "cons")`). The macro looks the name up via `doc-lookup` from
`tur/docstrings` and prints either the entry or a "No documentation found."
message.

Since `tur/` modules are auto-loaded, `doc` is globally available -- no
explicit import needed.

---

## Web REPL integration

The Try Turmeric web app exposes the same lookup table from the WASM build.
`src/web/wasm_glue.c` exports:

```c
EMSCRIPTEN_KEEPALIVE
const char *turi_doc_lookup(const char *name);
```

The frontend calls `turi_doc_lookup("cons")` after each `(doc ...)` eval and
renders the result in the doc panel as formatted HTML. Plain-text output also
goes to the console for parity with the CLI REPL.

Because the lookup goes through the WASM module itself (not a network fetch),
`(doc name)` works offline and in embedded deployments.

---

## CI and regeneration discipline

- `tur run wasm` depends on `tur run docs` so the WASM build always carries an
  up-to-date `stdlib/docstrings.tur`.
- Treat `docs/api/` and `stdlib/docstrings.tur` as build artifacts: regenerate
  them in the same commit as the docstring change, never in a follow-up.
- A non-`;;;` line resets the parser's docstring buffer. If a docstring is not
  showing up, check for a stray blank line or `;;` comment between it and its
  definition.

---

## See also

- `tools/gendocs.py` -- the generator script
- `stdlib/docstrings.tur` -- the auto-generated lookup table (do not edit)
- [REPL Guide](repl.md) -- using `(doc name)` interactively
