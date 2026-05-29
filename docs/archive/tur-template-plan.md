# Plan: `tur-template` -- ERB/EJS-style String Templating

> **Status:** Draft Plan
> **Last Updated:** 2026-05-28
> **Type:** Spice Design + Implementation Roadmap
> **Related:**
> - `docs/guides/developing-spices-guide.md` (spice authoring conventions)
> - `docs/tur-httpd-plan.md` (HTTP server spice)
> - `docs/tur-turist-plan.md` (Scotty-style micro-framework that integrates with this spice)

---

## Overview

`tur-template` is a standalone string-templating engine modeled on ERB / EJS.
It has no external dependencies and can be used in any Turmeric program -- a
CLI tool, code generator, email renderer, or as the view layer underneath
`tur-turist` HTTP responses.

It is one of three spices that together form a composable web stack for
Turmeric:

| Spice | Analogue | Depends on |
|---|---|---|
| `tur-template` | ERB / EJS | (none -- pure Turmeric) |
| `tur-httpd` | Mongoose / Civetweb | (none -- POSIX sockets + pthreads) |
| `tur-turist` | Haskell's scotty | `tur-httpd`, `tur-template` |

The three are deliberately separate so any layer can be used independently.

---

## Motivation

Turmeric already has first-class string operations but no composable way to
mix control flow and literal text. A minimal template engine eliminates the
`str-concat` soup that arises in code generators, email renderers, and
HTTP response bodies.

ERB/EJS were chosen as the model because:
- Syntax is widely known and tooling-friendly (syntax highlighters exist).
- The template engine itself requires no external C library.
- Semantics are simple enough to specify completely in one file.

---

## Syntax

```
<% ... %>    code block (result discarded; use for if/let/for)
<%= ... %>   expression block (result coerced to :cstr and emitted)
<%# ... %>   comment (removed entirely; never emitted)
<%%          literal <%  (escape)
```

Everything outside a tag is emitted verbatim.

### Example

```tur
(import template/render :refer [render render-file])

(let [tmpl "<h1><%= title %></h1>\n<% for [item items] %><li><%= item %></li>\n<% end %>"]
  (render tmpl {"title" "List" "items" (list "a" "b" "c")}))
; => "<h1>List</h1>\n<li>a</li>\n<li>b</li>\n<li>c</li>\n"
```

---

## Design

The engine is three passes over the template string, all in pure Turmeric
with one thin inline-C helper for the inner scan:

```
1. Lex:    cstr -> list<Token>   (TextChunk | CodeBlock | ExprBlock | Comment)
2. Parse:  list<Token> -> list<Node>  (Literal | Emit | IfNode | ForNode | LetNode)
3. Render: list<Node> x Env -> :cstr
```

`Env` is a `Map<:cstr :cstr>` from `tur/map`. All interpolated values are
strings; callers must convert before passing. This keeps the engine small and
avoids pulling in a reflection or dynamic-dispatch system.

### Supported control forms inside `<% ... %>`

| Form | Syntax |
|---|---|
| `if` | `<% if cond %>` ... `<% else %>` ... `<% end %>` |
| `for` | `<% for [x xs] %>` ... `<% end %>` |
| `let` | `<% let [x val] %>` ... `<% end %>` |

These forms mirror Turmeric's own syntax and are parsed by the template engine
as a small DSL (not eval'd as Turmeric code). This keeps `tur-template`
dependency-free and sandboxable.

### `render-file`

```tur
(render-file "/path/to/template.html.tur" env)
```

Reads the file, passes to `render`. File extension `.tur` is conventional but
not enforced.

---

## Module Layout

```
tur-template/
  build.tur
  src/
    template/
      token.tur       -- Token type; lex :: :cstr -> list<Token>
      parse.tur       -- Node type; parse :: list<Token> -> list<Node>
      render.tur      -- render :: list<Node> x Env -> :cstr
                         render-file :: :cstr x Env -> result<:cstr>
      env.tur         -- Env type alias; make-env, env-get, env-set
  tests/
    fixtures/
      basic/
        template.html.tur
        expected.txt
        main.tur
      if-else/
      for-loop/
      escaping/
```

---

## `build.tur`

```turmeric
(defpackage tur-template
  :name        "tur-template"
  :version     "0.1.0"
  :description "ERB/EJS-style string templating engine"
  :license     "MIT"
  :spices #{
    "test" #{:url "https://github.com/turmeric-lang/turmeric-spices"
            :ref "test-v0.1.0"
            :subdir "spices/test"
            :optional true}
  }
  :exports #{
    "template/render" ["render" "render-file"]
    "template/env"    ["make-env" "env-get" "env-set"]
  })
```

---

## Implementation Phases

| Step | Task |
|---|---|
| T1 | Scaffold `tur-template` in `../turmeric-spices/spices/template/` |
| T2 | Implement `token.tur`: lexer that produces `list<Token>` |
| T3 | Implement `parse.tur`: parser for `if/else/end`, `for/end`, `let/end`, emit |
| T4 | Implement `render.tur`: `render` and `render-file` with `Env = Map<:cstr :cstr>` |
| T5 | Add `env.tur`: thin `make-env`, `env-get`, `env-set` wrappers |
| T6 | Write fixture tests: basic, if-else, for-loop, escaping |
| T7 | Add docstrings (`;;;`) to all exported functions |
| T8 | Run `just docs` to include `tur-template` in the API reference |

---

## Open Questions

- **Eval mode:** Resolved -- `tur-template` will never gain a real-eval mode.
  The DSL stays sandboxed so the safety boundary is explicit. If real-eval is
  ever needed, it ships as a separate spice (`tur-template-eval`) rather than
  a flag on this one, so users opt in to the unsafe surface deliberately.
