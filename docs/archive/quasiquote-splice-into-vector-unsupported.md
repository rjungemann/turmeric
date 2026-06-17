---
title: Quasiquote `~@` splicing is not implemented inside vector/map/set literals -- only inside list context
severity: medium -- blocks any macro that synthesizes a `defstruct` field vector (or any `[...]`/`#map{}`/`#set{}` literal) from a computed list. Forces per-arity macro cascades. This is the *separate* limitation that still blocks collapsing the ECS `defworld--0..5` cascade into one variadic-over-components macro, even though the type-position unquote bug it was paired with is now fixed.
status: RESOLVED 2026-06-17. `~@` now splices inside vector, set, and map
  literals (and the defstruct field vector).  Regression fixture:
  tests/fixtures/quasiquote-splice-into-vector/.  NOTE: the downstream
  variadic `defworld` collapse is still blocked, but by a SEPARATE,
  pre-existing limitation now tracked in
  docs/reported/ct-macro-evaluator-no-function-call-in-splice.md -- the CT
  macro evaluator cannot call `map` or expand a nested macro inside a splice
  expression, so a template cannot *compute* the sequence it splices (it
  fails identically in list context, proving it is not this vector bug).
discovered: 2026-06-17
surfaced-by: turmeric-spices ECS work (E2d). The variadic `defworld` collapse anticipated by docs/archive/macro-template-type-position-rejects-unquoted-compound.md is still blocked here; the 0-5 arity cascade had to be kept.
---

> **RESOLVED 2026-06-17.** Fixed `ct_eval_quasiquote` to expand `~@`
> children for every sequence kind (the F_VEC / F_MAP / F_SET / map-literal /
> set-literal / row-literal branch previously mapped children 1:1 and never
> looked for `F_UNQUOTE_SPLICING`).  A shared `ct_qq_eval_seq_items` helper
> now drives both the list branch and the sequence-literal branches.  Also
> deferred the `#map{...}` even-arity / key-form parse-time checks
> (`reader.c`, TUR-E0280/E0282) when a slot is an unquote/splice, since the
> real slots are only known after expansion.  Proven for a defstruct field
> vector (whole-list and mixed literal+splice, with type annotations
> surviving), a `[...]` data vector, `#set{...}`, and `#map{...}`.  Full
> suite green (`1661 passed, 0 failed`).
>
> The variadic `defworld` collapse this report anticipated needs an
> additional, independent capability -- generating the spliced sequence with
> `map`/recursion at macro-eval time -- which is filed separately as
> `docs/reported/ct-macro-evaluator-no-function-call-in-splice.md`.

# `~@` splice unsupported inside a vector (defstruct field-vector) literal

## One-line summary

In a quasiquoted macro template, `~@expr` splices correctly into a **list**
`(...)` context but is silently mis-handled inside a **vector** `[...]`
(and map/set) context: the splice node is processed one-to-one as a single
element instead of being expanded, so the spliced value is never flattened
into the surrounding literal. Because a `defstruct` field list is a vector,
a macro cannot compute its fields with `~@(map ...)`.

## Minimal repros

Splice a computed field list into a defstruct's field vector:

```turmeric
(defmacro mk [name flds]
  `(defstruct ~name [~@flds]))

(defstruct A [x : int])
(mk W ([f1 : int] [f2 : int]))

(defn main [] : int 0)
```

```
$ ./build/tur check /tmp/repro.tur
repro.tur:5:7: error: defstruct field list: expected field name symbol
5 | (mk W ([f1 : int] [f2 : int]))
  |       ^^^^^^^^^^^^^^^^^^^^^^^
```

The realistic ECS shape fails earlier, at macro-eval time:

```turmeric
(defstruct Dense [n : int])

(defmacro defworld [name comps]
  `(defstruct ~name
     [gens : int
      ~@(map (fn [c] `[~c : (Dense)]) comps)]))

(defworld World (a b))
```

```
repro.tur:6:14: error: compile-time macro evaluation expected a form value
6 |       ~@(map (fn [c] `[~c : (Dense)]) comps)]))
  |              ^^^^^^^^^^^^^^^^^^^^^^^
```

For contrast, the same `~@` in a **list** context expands fine
(`(defmacro call-all [& args] \`(do ~@args))`), and unquoting a *whole
literal vector* works -- `(defmacro mk [name fieldvec] \`(defstruct ~name
~fieldvec))` with `(mk W [a : int b : int])` compiles and runs. Only
*splicing into* a vector is broken.

## Root cause

`src/compiler/elab_macros.c`, `ct_eval_quasiquote`.

The `F_LIST` branch (`elab_macros.c:131-182`) explicitly detects
`F_UNQUOTE_SPLICING` children, evaluates them, and flattens a list-valued
result into the output (the `splice[i]` / `n_out` bookkeeping).

The `F_VEC` / `F_MAP` / `F_SET` / `F_MAP_LITERAL` / `F_SET_LITERAL` /
`F_ROW_LITERAL` branch (`elab_macros.c:184-199`) does **not**:

```c
case F_VEC:
case F_MAP: /* ... */ {
    uint32_t n_in = f->as.list.len;
    Form **items = arena_alloc(..., n_in * sizeof(Form *));
    for (uint32_t i = 0; i < n_in; i++)
        items[i] = ct_eval_quasiquote(env, f->as.list.items[i]);  /* 1:1 */
    /* ... rebuild same-arity literal ... */
}
```

Every element is mapped 1:1 through `ct_eval_quasiquote`. When an element
is an `F_UNQUOTE_SPLICING`, it hits the leaf case at
`elab_macros.c:127-130`, which evaluates the inner expr and funnels it
through `ct_value_to_form`. A list-valued result (e.g. `map`'s cons list)
is not a `CT_VAL_FORM`, so `ct_value_to_form` (`:114-119`) raises
"compile-time macro evaluation expected a form value". When the spliced
value *is* a single form (the `~@flds` repro), it gets dropped in as one
vector element -- yielding the "expected field name symbol" defstruct
error because the field vector now holds a nested list form where a name
symbol was expected.

## Proposed fix

Hoist the `F_LIST` branch's splice handling into a shared helper and use
it for the `F_VEC` / map / set / row cases too: scan children for
`F_UNQUOTE_SPLICING`, evaluate each, and flatten list-valued results into
the rebuilt literal (computing `n_out` the same way). The only difference
per tag is the final constructor (`form_vec` / `form_map` / `form_set` /
`form_map_literal` / ...). Map/set literals should reject a splice that
produces an odd element count for maps, mirroring existing literal-arity
checks.

## Validation when fixed

- The minimal repros compile, expand, and run.
- The ECS `defworld` macro in
  `../turmeric-spices/spices/ecs/src/ecs/world.tur` collapses from
  `defworld--0`..`defworld--5` back to one variadic-over-components form
  (the goal that `docs/archive/macro-template-type-position-rejects-unquoted-compound.md`
  anticipated but which remained blocked here after the type-position fix
  landed).
- A new in-tree fixture pins splicing into a vector field list (and ideally
  one into `#map{...}` / `#set{...}`).

## Cross-references

- `docs/archive/macro-template-type-position-rejects-unquoted-compound.md`
  -- the type-position unquote bug (RESOLVED); this is the remaining,
  independent blocker for the variadic `defworld` collapse it described.
- `docs/archive/history/quasi-quote-defstruct-field-type-unquote-rejected.md`
  -- the earlier, related (now-resolved) defstruct field-*type* unquote
  fix; that one handled unquote-in-type-position, not `~@` splice into the
  field *vector*.
- `docs/reported/defstruct-bare-user-type-field-reads-back-as-int-carrier.md`
  -- the other gap from the same E2d session.
