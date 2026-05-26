# Cleanup Plan: Stop Showing `cons` in Public Examples

## Why this matters

New Turmeric users meet the language through spice `README.md`s and the
top-level docs. Today, the first runnable example in several READMEs looks
like this:

```turmeric
(glsl-fragment-shader "330 core"
  (cons (glsl-output "FragColor" ":vec4") 0)
  (glsl-set! "FragColor" "vec4(1.0, 0.5, 0.2, 1.0)"))
```

```turmeric
(cons (tick-grid)
  (cons (axes)
    (cons (function (fn [x :float] :float (* x x))
                    0.0 1.0 100)
          0)))
```

```turmeric
(let [a (frame "x" (cons 1.0 (cons 2.0 (cons 3.0 0))))
      b (frame "x" (cons 4.0 (cons 5.0 (cons 6.0 0))))]
  ...)
```

Three problems:

1. **The literal `0` is a footgun.** A reader has to be told that `0` is
   how `nil` is spelled at runtime. Nothing in the example hints at that.
2. **Reading order is inverted.** A list of three items needs four lines
   of right-cascading indentation, and the elements appear in source
   order but parentheses nest backwards.
3. **It looks like 1959 LISP.** People walk away thinking "this language
   makes me hand-build linked lists for ordinary data" and quit. It's a
   first-impression bug, not a correctness bug.

The goal is to remove every `cons` chain from user-facing examples in
favor of something that reads like data.

## Audit (current state)

Spice READMEs containing `cons` chains as runtime list construction:

- `../turmeric-spices/spices/glsl/README.md` (outputs list)
- `../turmeric-spices/spices/plot/README.md` (layers list)
- `../turmeric-spices/spices/frame/README.md` (group-by / outs / ins / tags)
- `../turmeric-spices/spices/stats/README.md` (column data)
- `../turmeric-spices/spices/c-dsl/README.md` (statements, params)
- `../turmeric-spices/spices/scscm/README.md` (passing reference to "cons list", mostly fine)
- `../turmeric-spices/spices/signal/README.md` (occurrences are `constant`, not `cons` — no action)
- `../turmeric-spices/spices/tidal/README.md` (occurrences are `constant` / prose mentioning "cons list of events")

(False positives like `constant`, `consider`, `consumers`, and prose that
discusses the data structure itself are excluded.)

Top-level docs to sweep: `docs/guides/**/*.md`, top-level `README.md`,
and `docs/design/**` for the same anti-pattern.

## What `cons` actually competes with today

Already in the codebase:

| Form | Where it lives | Runtime or compile-time? |
|------|----------------|--------------------------|
| `(cons x rest)` with `0` for nil | runtime builtin | runtime cons cell |
| `(tcons h t)` / `(tnil)` | `stdlib/list.tur` | runtime, typed `List[A]` |
| `(list ...)` | `src/compiler/elab_macros.c:338` | **compile-time only** — builds an `F_LIST` AST node, not a runtime list |
| `` `(...) `` quasiquote + `~`/`~@` | reader (`src/compiler/reader.c:567`) | **compile-time only** — same |
| `(vec ...)` literal | first-class | runtime `Vec` |
| `Vec` from `stdlib/vec.tur` | stdlib | runtime growable array |

The key constraint: **`list` and quasiquote do not produce runtime
cons-list values today.** Both produce macro-AST forms. So we cannot
just write `(list a b c)` in a README and have it work as a drop-in for
`(cons a (cons b (cons c 0)))` — not without changes.

## Decision tree for each example

For every `cons` chain in docs, classify the call site:

### Case A — the API actually wants a `Vec`

Many of these examples (`tick-grid`/`axes`/`function` as plot layers;
`glsl-output` records; `c-stmts` body) really are heterogeneous
sequences whose length is known at the call site. A `Vec` literal is
the natural fit:

```turmeric
(plot (vec (tick-grid)
           (axes)
           (function (fn [x :float] :float (* x x)) 0.0 1.0 100)))
```

If the underlying spice function takes a cons-list `:int`, change its
signature (or add an overload) to accept `Vec[T]`. This is the
preferred outcome — vectors are how almost every other language spells
"a few things in order."

### Case B — the API genuinely wants a singly-linked list

Some functions (recursive descent over a list, structural sharing,
infinite streams) really want a cons list. For those, introduce a
runtime `list` macro:

```turmeric
;; in stdlib/list.tur (or stdlib/macros.tur):
(defmacro list [& xs]
  (if (zero? (length xs))
    0
    (cons 'cons (cons (head xs)
                      (cons (cons 'list (tail xs)) 0)))))
```

(Expansion: `(list a b c)` → `(cons a (cons b (cons c 0)))`.)

Then READMEs say:

```turmeric
(plot (list (tick-grid)
            (axes)
            (function ...)))
```

The user never sees the `0`-as-nil convention in a quick-start.

### Case C — quasiquote (deferred / non-goal for this pass)

Quasiquote in user code is appealing but currently produces AST forms.
Promoting `` `(a b c) `` to a runtime list literal is a *bigger
language change* — it would shift the meaning of an existing reader
form. Out of scope here; revisit only if there's separate demand.

## The macro vs. Vec decision (recommendation)

For each example, default to **Case A (`Vec`)** unless the API has a
documented reason to be a cons list. Reasons to keep cons:

- The API is `O(1)` prepend in a tight loop.
- The API recurses structurally with `head`/`tail`.
- The function is documented as part of a Lisp-style data-structure
  story (e.g. `scscm`).

Otherwise: change the spice's public surface to take `Vec`, and update
the README. This makes the README better *and* makes the API more
approachable.

For the cases that stay cons, ship the `list` macro from Case B so the
README never shows a bare `0`.

## Concrete steps

1. **Land the `list` macro** (`stdlib/list.tur`, or `stdlib/macros.tur`).
   Add docstring + at least one fixture under
   `tests/fixtures/typed/list-macro/` exercising 0, 1, and N args.
   Add to `stdlib/docstrings.tur` via `just docs`.

2. **Audit spice APIs**. For each call site below, decide A vs. B:
   - `glsl-fragment-shader` outputs arg → likely A (`Vec`)
   - `plot` layers arg → A (`Vec`); plot layers are heterogeneous values
   - `group-by` / `frame` column-spec args → A (`Vec`); these are tabular
   - `frame` column data → A (`Vec[float]`); a column is a vector by definition
   - `c-stmts` body, `c-fn` params → A (`Vec`); these are typed sequences
   - anything in `scscm` → B; it's a Scheme implementation, cons is the point

   Record the decision in `../turmeric-spices/<spice>/CHANGELOG.md` or
   the spice's own plan doc, not here.

3. **Migrate APIs marked A**. For each spice, either:
   - change the function signature to `Vec[T]` and update internals, or
   - keep the cons signature but ship a thin `Vec`-accepting wrapper
     (e.g. `plot-v` / `plot/vec`) and use the wrapper in the README.

   Prefer the first option; the second is a fallback when an in-place
   change would break dependents.

4. **Rewrite the READMEs**. For every file in the audit list:
   - Replace `cons ... 0` chains with `vec` or `list` per the decision.
   - Re-run any `tur check` / `tur run` examples that READMEs document
     so the new snippets actually compile.
   - Re-render sweet-exp variants so they match the s-expr version.

5. **Sweep top-level docs**. Same rewrite for `docs/guides/**`,
   `README.md`, and any `docs/design/**` example that uses `cons` as
   a list constructor (as opposed to talking *about* cons cells).

6. **Add a style note** to `docs/guides/style-guide.md` (or the closest
   existing style doc): "Don't show `cons ... 0` chains in examples;
   use `vec` for runtime sequences or `list` when a cons list is
   actually required." Reference this plan from there.

## Non-goals

- Removing `cons` from the language. It stays — it's the underlying
  primitive and `scscm` / interpreter macros depend on it.
- Promoting quasiquote to a runtime list literal. That's a separate
  design conversation.
- Adding N-ary tuples. None of the audit examples are actually
  heterogeneous at the type level — they all collapse to `:int` handles
  or homogeneous `Vec`. The tuple question is tracked separately in
  `docs/tuple-type-plan.md`; do not block this cleanup on it.
- Changing `*args*` or `g_tur_args`. The CLI-args rule in `CLAUDE.md`
  is independent of this cleanup.

## Out-of-band risks

- The `list` macro name might collide with the compile-time `list`
  builtin in `elab_macros.c:338`. Verify the runtime macro shadows the
  CT builtin only inside runtime call positions; if there's any
  ambiguity, name the macro `list*` or `lst` and adjust.
- Spice API changes are user-visible. Any `cons`-to-`Vec` signature
  change needs a minor-version bump in the spice's `build.tur` and a
  CHANGELOG entry.

## Success criteria

- `rg -n '\bcons\b.* 0\)' ../turmeric-spices/spices/**/README.md` returns
  zero hits (modulo prose mentioning cons by name).
- Every quick-start in a spice README compiles via `tur run` against
  the spice's own `build.tur`.
- A new reader can read a quick-start without being told what `0` means.
