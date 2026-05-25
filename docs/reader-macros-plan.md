# Plan: User-defined `#`-prefixed reader macros

> **Status:** Draft Plan
> **Last Updated:** 2026-05-24
> **Type:** Compiler / Reader / Language

---

## Overview

The reader already does ad-hoc `#`-dispatch for a handful of built-in
forms: `#{...}` (map), `#s(...)` (set), `#r{...}` (range), `#[...]`
(attribute), `#?(...)` (reader conditional), `#;datum` (datum comment),
`#|...|#` (block comment), and `#lang` (language directive). Every one
of these is a hardcoded branch in `read_form` in
`src/compiler/reader.c:1510-1559`.

This plan adds a registry so users (and spices) can register their own
reader macros in two shapes:

1. **Bare identifier** -- `#foo` reads as a single token. The handler
   receives nothing; the expansion is a fixed datum (or a parametric
   one if the registration carries data).
2. **Identifier + delimited body** -- `#foo[...]`, `#foo{...}`,
   `#foo(...)` etc. The handler receives the body, either as a *raw
   string* (verbatim slice of source) or as a *parsed datum* (recursive
   read until matching close-delim).

Collisions are prevented by reserving the built-in prefixes and
checking at registration time. The dispatch path becomes a lookup, not
a switch.

This is a language-extension feature aimed at letting spices ship
domain syntax (`#json{...}`, `#glsl{...}`, `#sql{...}`,
`#mat4{...}`, etc.) without forking the reader.

---

## Non-goals

- **Phase-separated `define-syntax`.** This plan does **not** allow
  arbitrary Turmeric code to run during read. Doing so requires a
  bootstrapped compile-time interpreter and a phase-separation story
  for module loading -- both large items deferred to a future plan.
- **Custom delimiter pairs.** Bodies use one of the existing matched
  delimiters (`( )`, `[ ]`, `{ }`). No `#foo<...>` or `#foo|...|`.
- **Macros that consume across whitespace without a delimiter.**
  Either you're a bare identifier or you take a delimited body --
  nothing in between. This keeps the grammar LL(2) on the dispatch
  prefix.
- **Replacing or shadowing built-ins.** The reserved list is final;
  attempts to register `#lang`, `#?`, `#;`, `#|`, `#{`, `#s(`,
  `#r{`, or `#[` fail at registration time.

---

## Surface syntax

### Bare identifier form

```turmeric
#nan          ; expands to a registered datum (e.g. (f64-nan))
#now          ; expands to (current-time-ns) or similar
```

The identifier follows the same rules as a normal Turmeric symbol
(letters, digits, the usual punctuation set), but cannot contain a
delimiter.

### Delimited-body form

```turmeric
#json{ "name": "ada", "age": 36 }       ; raw-string body
#mat4[ 1 0 0 0
       0 1 0 0
       0 0 1 0
       0 0 0 1 ]                         ; datum body
#sql( select * from users where id = ?1 ); raw-string body, parens
```

The handler is registered with a *body mode*:

| Mode | Reader behavior | Use case |
|------|-----------------|----------|
| `raw` | Slice source from open-delim+1 to matched close-delim-1, balanced. Keep verbatim. | Embedded foreign syntax (JSON, SQL, GLSL, regex) |
| `datum` | Recursive `read_form` until matched close-delim. Handler gets a list of forms. | DSL whose body is still Turmeric-shaped (matrix literals, struct shorthand) |

In `raw` mode the reader still has to track nesting to find the
correct close-delim, so unbalanced delimiters inside a body need an
escape convention (proposed: backslash-escape the delim, same as
strings).

---

## Registration API

```turmeric
;;; reader-macro/define -- register a custom #-dispatch handler.
;;;
;;; Parameters:
;;;   name        -- the identifier following '#', as a symbol
;;;   body-mode   -- :none | :raw | :datum
;;;   expander    -- a function that takes the body (or nothing) and
;;;                  returns the expansion as a Form
;;;
;;; Errors:
;;;   - if `name` collides with a reserved prefix
;;;   - if a macro with the same (name, delimiter) is already registered
```

Open question: does `expander` run at *read time* (requires
phase-separation) or is it captured as a template that the reader
expands directly? **Recommended first cut:** template-only, no
arbitrary code. A template is a Form with `$body` and `$1`, `$2`, ...
splice points. This sidesteps the bootstrap question and still covers
the common cases (`#json{...}` -> `(parse-json "...")`,
`#mat4[...]` -> `(make-mat4 ...)`).

A later plan can add `expander : Form -> Form` once compile-time
evaluation lands.

---

## Composition with regular macros

Reader macros in this design are deliberately *dumb*: pure tree
substitution, no compile-time evaluation. They run as a pre-pass that
produces `Form*` trees; the regular-macro pass runs later and walks
those trees unchanged.

The consequence is that **a reader macro can expand to a call that is
itself a regular macro**, and the regular macro picks up the
expansion automatically. The reader doesn't need to know anything
about the macro it's emitting -- it just splices a Form, and the
later pass does the rest.

```turmeric
(reader-macro/define 'when+ :datum '(when (> $1 0) $body))

#when+[x (println "positive")]
; after reader pass:  (when (> x 0) (println "positive"))
; after macro pass:   (if (> x 0) (do (println "positive")) nil)
```

This is load-bearing for the plan:

- **Reader macros stay simple.** Anything that needs computation
  (validation, conditional structure, hygiene) is pushed into a
  regular macro that the template expands into. The reader half of
  the system never needs to grow.
- **`#sql{...}` -> `(sql/query "...")` works even if `sql/query` is
  a real macro** that validates the query string, parses bind
  variables, and emits a prepared statement -- none of that has to
  live in the reader.
- **The split between "syntax" and "semantics" stays clean.** The
  reader is responsible for *shape* (how the text becomes a tree);
  regular macros are responsible for *meaning* (what the tree
  compiles to). Reader macros that try to do both end up as
  miniature compilers; templates that emit macro calls don't.

This is also why function expanders (RM5) are not urgent: most of
the power they'd add is already reachable by templating into a
regular macro.

---

## Where the changes go

### 1. Dispatch table

`src/compiler/reader.c:1510-1559` becomes:

```c
/* Try user-registered reader macros first (after reserved prefixes
 * have been ruled out via the registry's own bookkeeping). */
if (c == '#') {
    Form *m = try_read_user_macro(r);
    if (m || r->error) return m;
    /* fall through to built-ins below */
}
```

`try_read_user_macro` peeks the identifier after `#`, looks it up in
the registry, optionally peeks the next char for a delimiter, and
either reads the body in the requested mode or commits to the bare
form.

The built-in branches stay as-is and run *after* the user lookup --
but since the registry rejects any name that would shadow a built-in,
order doesn't matter for correctness; user-first is just faster for
the common case.

### 2. Registry

A new file, `src/compiler/reader_macros.{c,h}`:

- A small open-addressed hash from `(name, delim)` -> handler record.
- Reserved-prefix table populated at startup.
- Public C API: `reader_macros_register(name, delim, mode, template,
  diag_span)`.
- Public Turmeric binding: `reader-macro/define` from
  `stdlib/reader.tur` (new file).

### 3. Loading order

Reader macros are a *parse-time* construct, so they have to be
visible before the file that uses them is read. Two options:

- **(a) Per-file `#use-reader-macros <module>` directive**, modelled
  on `#lang`. Has to appear before any form. The compiler reads the
  named module's reader-macro registrations first, then resumes
  reading the current file with the registry populated.
- **(b) Spice-level manifest entry** in `build.tur`, e.g.
  `:reader-macros [my-spice/json my-spice/glsl]`, applied to every
  source file in the spice.

Recommend shipping (a) first -- it's local, explicit, and doesn't
require build-system plumbing. (b) can land later as sugar.

### 3a. Loading semantics (cross-file)

Once macros span multiple files (manifest entry, `#use-reader-macros`,
or in-source defines inside an imported module), the user-facing
contract is:

- **Single shared registry per compile.** Every file in the compile --
  the entry file, all imports (`elab_module.c`), and all `(load ...)`
  targets (`elab_toplevel.c`) -- reads against the same
  `ReaderMacroRegistry`. A macro registered in any one of them is
  visible to every subsequent read.
- **`import` and `(load ...)` are equivalent for macro purposes.**
  They use the same registry; they differ only in how the loaded
  forms are spliced into the rest of elaboration. A
  `(reader-macros/define ...)` is hoisted into the shared registry
  identically either way.
- **Order matters.** Macros must be registered *before* the file that
  uses them is *read*. In particular:
  - In-source `(reader-macros/define ...)` works for any *subsequent*
    forms in the same file (single-pass read).
  - A `#use-reader-macros "path"` directive works for everything that
    comes after it in the same file.
  - `(import lib/syntax)` followed by `(import lib/user)` works
    when `lib/syntax` registers a macro that `lib/user` uses --
    imports are processed in source order, so `lib/syntax` is fully
    read (and its defines registered) before `lib/user` is opened.
  - A `(import lib/syntax)` cannot make macros usable in the
    importing file's *own body* -- the entry file has already been
    fully read by the time the elaborator walks its imports. Use the
    spice-manifest `:reader-macros` entry or a `#use-reader-macros`
    directive at the top of the entry file instead.
- **Compile-time registration is strict.** Two
  `(reader-macros/define ...)` forms with the same `(name, delim)` in
  the same compile is an error with a "previously registered here"
  note. The REPL/eval registry deliberately stays non-strict so a
  user can iteratively redefine a macro across turns -- and so the
  REPL's accumulated-source replay (`src_acc`) doesn't trip on its
  own retained defines.
- **Diagnostics carry an import chain.** A reader error inside an
  imported module attaches a `note: while loading module 'X'` at the
  import site so the user can navigate back from the leaf parse
  error to the import that triggered it.

### 4. Error surface

New diagnostics:

- `reader macro '#foo' not registered` (when a `#`-token doesn't match
  any built-in or user macro, before the current generic error)
- `reader macro '#foo' expects '[' body, got '{'` (delimiter mismatch)
- `reader macro '#foo' cannot shadow built-in '#foo'` (at registration)
- `unterminated body for '#foo' (missing '}')` (balanced-scan failure
  in `raw` mode)

---

## Phased rollout

### RM0 -- Reserved-prefix table + registry skeleton

- Add `src/compiler/reader_macros.{c,h}`.
- Populate reserved list from existing branches in `read_form`.
- No-op dispatch (no user macros yet); ensures the lookup path
  doesn't change behavior.
- Test: existing reader tests still pass.

### RM1 -- Template-only `raw` body

- Implement `try_read_user_macro` for delimited `raw` bodies.
- Implement balanced-scan with backslash escape.
- Add `reader-macros/define` template form (no expander function).
- `#use-reader-macros` directive.
- Fixture: `tests/fixtures/reader/macros-raw/` with `#json{...}` ->
  `(parse-json "...")`.

### RM2 -- `datum` body mode

- Recursive read until matched close-delim.
- Splice points (`$1`, `$2`, ..., `$body`) in templates.
- Fixture: `#mat4[ ... 16 floats ... ]` -> `(make-mat4 ...)`.

### RM3 -- Bare-identifier form

- `#foo` with no body, template-expanded.
- Fixture: `#nan`, `#now`.

### RM4 -- Spice-level manifest entry

- `build.tur` accepts `:reader-macros [...]`.
- Applied implicitly to every file in the spice.
- Per-file directive still works and overrides.

### RM5 (deferred) -- Function expanders

- Once compile-time eval is available, allow
  `expander : Form -> Form` instead of just templates.
- Out of scope for this plan; tracked separately.

---

## Open questions

1. **Splice-point syntax in templates.** *Resolved (RM1-RM3): kept
   `$1`/`$body`.* The sweet-exp collision is theoretical -- `$`
   triggers as rest-of-line only at the *start* of a sub-expression,
   and a quoted template body never appears there. `%1`/`%body` and
   the quasiquote form remain viable if the collision turns out to
   matter in practice.
2. **Identifier character class.** *Resolved (RM-Q2): loosened to
   match the symbol reader.* Names like `#if+`, `#?some`, `#<-bind`
   dispatch. One carve-out: `#` is not a continuation byte, so
   `#foo#bar` still reads as two adjacent macros.
3. **Hygiene.** Templates are pure substitution. **Capture is
   possible** -- a template that introduces a local binding `x` and
   a body that also uses `x` will alias them. For RM1-RM3 the
   guidance is: keep templates small and free of `let`/`fn`-introduced
   names, or rename to a private prefix (e.g. `__rm_x`) by hand. The
   proper fix is the deferred function-expander work (RM5), which
   can rename binding-introducing symbols at expansion time.
4. **Source spans.** Still open. Currently the expanded form
   inherits the spans recorded when the template was *registered*,
   so diagnostics on the expansion point at the
   `(reader-macros/define ...)` line rather than the `#foo{...}` use
   site. Tracked as a follow-up task.
5. **REPL story.** Still open. Today's registry is per-`read_all`
   call, so a REPL turn discards prior `(reader-macros/define ...)`s.
   Tracked as a follow-up task; the design intent is "one long file
   with a mutable shared registry."

---

## Risk and scope

- **Risk: low** for RM0-RM3 (template-only). The reader changes
  are localized to one dispatch site; the registry is new code with
  no callers until macros are registered.
- **Risk: medium** for RM4 (build.tur integration touches the cmake
  pipeline and `tur build <dir>` driver).
- **Risk: high** for RM5 (function expanders) -- deferred for that
  reason.

LOC estimate: ~400 lines for RM0-RM3, plus ~150 lines of fixtures
and tests.
