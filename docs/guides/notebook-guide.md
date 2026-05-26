# Turmeric Notebook Guide

`tur-notebook` brings literate-programming `.tur.md` notebooks to the Turmeric
ecosystem -- think Jupyter, but backed by the Turmeric evaluator with a
terminal-first TUI, standalone HTML export, and a scriptable `exec` mode.

---

## Installing

Clone `turmeric-spices` next to your `turmeric` checkout:

```sh
git clone https://github.com/rjungemann/turmeric-spices ../turmeric-spices
```

Declare the spice in your project's `build.tur`:

```turmeric
:spices {
  "notebook" {:url    "https://github.com/rjungemann/turmeric-spices"
              :ref    "notebook-v0.1.0"
              :subdir "spices/notebook"}
}
```

Then run `tur add` or `tur build` to fetch and link it.

---

## Quick Start

### Create a new notebook

```sh
nb new my-notes.tur.md
```

This writes a starter `.tur.md` file with a prose heading and one
`turmeric` code fence.

### Open the TUI

```sh
nb tui my-notes.tur.md
```

Navigate cells with `j`/`k`, edit in your `$EDITOR` with `e`, evaluate with
`Enter`, and quit with `q` (you will be prompted if there are unsaved changes).
Press `?` for the full key binding list.

### Run cells non-interactively

```sh
# Evaluate every cell and print outputs
nb exec --all my-notes.tur.md

# Evaluate from a specific cell onward
nb exec --cell my-cell-id my-notes.tur.md
```

### Export to HTML

```sh
nb export my-notes.tur.md -o my-notes.html
```

Produces a self-contained HTML file with the vendored stylesheet.

### Render to terminal markdown

```sh
nb render my-notes.tur.md
```

---

## Notebook File Format

A notebook is a Markdown file where Turmeric code blocks are designated with
an info string that starts with `turmeric`:

````markdown
# My Notebook

Some prose.

```turmeric {id=setup}
(def x 42)
```

```turmeric {id=use-x depends=setup}
(+ x 1)
```
````

### Cell attributes

Attributes appear in a `{...}` block after `turmeric` on the opening fence:

| Attribute | Type | Description |
|-----------|------|-------------|
| `id` | string | Unique cell identifier (required for `exec --cell` and caching) |
| `eval` | `true`/`false` | Set to `false` to skip evaluation (default: `true`) |
| `depends` | string | Space-separated list of cell ids this cell depends on |
| `cache` | `true`/`false` | Cache output; re-use unless dependencies change (default: `false`) |
| `error` | `continue` | Continue evaluating subsequent cells if this cell errors |

---

## Cell Types

### Turmeric cells

Cells with the `turmeric` language tag are evaluated by the embedded Turmeric
interpreter session. All cells in a notebook share one session, so bindings
defined in earlier cells are visible in later ones.

### Prose cells

Everything outside a code fence is prose. Prose is rendered as Markdown in
the TUI and included verbatim in the HTML export.

---

## exec Subcommand

`nb exec` is the non-interactive entry point for running notebooks from
scripts, CI, or editor integrations.

```sh
# Evaluate all cells
nb exec --all notebook.tur.md

# Evaluate from a named cell onward
nb exec --cell analysis notebook.tur.md
```

Outputs are printed to stdout in the format `[<cell-id>] => <repr>`.
Error outputs are printed to stderr.
Exit code is 0 on success, 1 on evaluation or I/O error.

---

## new Subcommand

```sh
nb new my-notebook.tur.md
```

Writes a minimal starter notebook to the given path. Refuses to overwrite an
existing file (exit code 1 if the path already exists).

---

## Image Hook (NB11)

Cells can advertise image files to the TUI via the image hook convention:

```turmeric
(import notebook/image :refer [image-hook-record-path])

;; After generating /tmp/plot.png:
(image-hook-record-path "/tmp/plot.png")
```

The TUI reads the advertised paths and displays the images inline using:

- **Kitty graphics protocol** -- if `$TERM` is `xterm-kitty`
- **iTerm2 inline images** -- if `$TERM_PROGRAM` is `iTerm.app`
- **Text fallback** -- prints the file path for all other terminals

The image hook works by printing a `__NB_IMG__: <path>` marker to stdout,
which the session evaluator captures and strips before returning output.

---

## TUI Key Bindings

| Key | Action |
|-----|--------|
| `j` / `k` | Move down / up |
| `e` | Edit focused cell in `$EDITOR` |
| `Enter` | Evaluate focused cell |
| `o` | Toggle focused-output view |
| `i` | Insert cell below |
| `d` | Delete focused cell |
| `p` | Paste cell below |
| `/` | Start search |
| `n` / `N` | Next / previous search match |
| `?` | Show help overlay |
| `q` | Quit (prompts if unsaved changes) |

Custom key bindings can be provided with `--keybindings <file.tur>`, which is
merged onto the built-in map.

---

## Further Reading

- [`docs/notebook-spice-plan.md`](../notebook-spice-plan.md) -- full milestone plan
- [`../turmeric-spices/spices/notebook/README.md`](https://github.com/rjungemann/turmeric-spices/blob/main/spices/notebook/README.md) -- spice README
- [`developing-spices-guide.md`](developing-spices-guide.md) -- how to develop and publish spices
