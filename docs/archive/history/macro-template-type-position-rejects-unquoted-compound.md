---
title: Macro template parser rejects `~(first comps)` in type-position; only `~directparam` is accepted
severity: medium -- forces macro authors to per-arity-split any defmacro that needs to project a type-parameter into a defstruct field type or fn signature
status: RESOLVED 2026-06-17. The `F_TYPE_ANN` case in `ct_eval_quasiquote`
  (`src/compiler/elab_macros.c:202-209`) now recurses into its payload, so an
  unquoted compound like `~(first comps)` inside a `: (Box ~(first comps))`
  annotation is evaluated at expansion time instead of reaching
  `type_expr_from_form` as a raw unquote. The report's minimal repro compiles
  on current HEAD (`tur check` clean). NOTE: the *downstream* goal of collapsing
  the ECS `defworld--0..5` cascade into one variadic-over-components macro is
  still blocked, but by a SEPARATE limitation now tracked in
  `docs/reported/quasiquote-splice-into-vector-unsupported.md` (`~@` splice into
  a vector literal is unimplemented), not by this type-position bug.
discovered: 2026-06-16
surfaced-by: E2d-P1+P5 migration in turmeric-spices (the (defworld Name [Comps]) -> per-arity (defworld--N Name Comp1 ... CompN) split was forced by this gap)
---

> **RESOLVED 2026-06-17.** The type-position unquote described below now
> compiles. The remaining blocker for the variadic `defworld` collapse is
> the independent `~@`-splice-into-vector gap, filed separately as
> `docs/reported/quasiquote-splice-into-vector-unsupported.md`.

# Macro template parser rejects unquoted compound expressions in type position

## One-line summary

In a `(defmacro ...)` template, `~directparam` works in a type-application
slot like `(Box ~c1)`, but `~(first comps)` and other unquoted compound
expressions are rejected at template parse time with `unsupported type
expression form (expected symbol, keyword, or list)`. This forces macro
authors who project a type into a defstruct field or a fn signature to
either (a) split the macro into per-arity helpers that take direct params,
or (b) drop the macro and hand-roll the defstruct.

## Minimal repro

```turmeric
(defopaque Box [A] :int)

(defmacro mk-box-field [name comps]
  `(defstruct ~name [field : (Box ~(first comps))]))

(defstruct Pos [x : int])

(mk-box-field W (Pos))

(defn main [] : int 0)
```

Run:

```
$ ./build/tur run /tmp/repro.tur
/tmp/repro.tur:4:35: error: unsupported type expression form (expected symbol, keyword, or list)
1 | (defopaque Box [A] :int)
2 |
3 | (defmacro mk-box-field [name comps]
4 |   `(defstruct ~name [field : (Box ~(first comps))]))
  |                                   ^^^^^^^^^^^^^^
```

The error fires at **template parse time** (when the macro definition is
elaborated), not at expansion time. So the macro body never gets a chance
to evaluate `(first comps)`.

The direct-parameter spelling works fine:

```turmeric
(defmacro mk-box-field [name c1]
  `(defstruct ~name [field : (Box ~c1)]))   ;; compiles, expands correctly
```

## Where this bites

Macros that emit `defstruct` field types or fn signatures parameterized
by a list of caller-supplied component/type names — e.g. an ECS
`defworld`:

```turmeric
;; Wanted (variadic over components):
(defmacro defworld [name comps]
  `(defstruct ~name
     [gens : int
      ~@(map (fn [c] `[~c : (Dense ~c)]) comps)]))

;; Forced workaround (per-arity helpers):
(defmacro defworld--1 [name c1]
  `(defstruct ~name [gens : int  ~c1 : (Dense ~c1)]))
(defmacro defworld--2 [name c1 c2]
  `(defstruct ~name [gens : int  ~c1 : (Dense ~c1)  ~c2 : (Dense ~c2)]))
;; ... and so on up to whatever max arity the API supports.
(defmacro defworld [name comps]
  (if (empty? comps)
    `(defworld--0 ~name)
    (if (empty? (rest comps))
      `(defworld--1 ~name ~(first comps))
      ;; ... cascading arity dispatch
      )))
```

This is the exact shape the unsized `defworld` already uses (per the
session that surfaced this, the macro had to be re-split into
`defworld--0`..`defworld--5` to land typed storage fields).

## Why it matters

Beyond the awkward per-arity split, the workaround:

- Caps the supported arity at whatever the helper set covers. Adding a
  6th component to an ECS world requires extending the macro module.
- Breaks for mixed-type emission. The session's `filter-with-without`
  test had to drop the `defworld` macro entirely and hand-roll a
  `defstruct` with mixed `(Dense Pos)` / `Tag Player` / `Tag Dead`
  fields, because the per-arity helpers all assumed a uniform
  `(Dense Comp)` template.
- Defeats macro composability — a macro that wants to delegate to
  `defworld` while computing the component list dynamically cannot
  pass it as a vec; it has to inline the per-arity dispatch itself.

Variadic HKT rows shipped in turmeric 0.20.0 for value-level macros;
the macro template parser apparently hasn't caught up for type-position
splices.

## Root cause (suspected, not verified)

Type-expression elaboration in the macro template path
(`type_expr_from_form` in `src/compiler/elab_types.c`, ~line 1740) walks
the parsed Form looking for `F_SYM` or `F_LIST` heads. The unquoted form
`~(first comps)` is parsed as an unquote node whose payload is itself a
list — the type-expr walker presumably doesn't follow the unquote into
its payload, just rejects the unquote-headed form at the syntactic
level.

A fix would either:
1. Defer type-expression validation until after macro expansion
   completes (so the unquote produces a real type-shape that the walker
   can then accept), or
2. Teach the type-expression walker to recognize unquote nodes and
   pre-resolve them via the macro-expansion context.

Option 1 is the structurally clean fix; option 2 is narrower but
preserves the early-error behavior.

## Validation when fixed

- The minimal repro above compiles, expands `(mk-box-field W (Pos))`
  to `(defstruct W [field : (Box Pos)])`, and runs cleanly.
- The ECS `defworld` macro in `../turmeric-spices/spices/ecs/src/ecs/world.tur`
  can be collapsed from `defworld--0`..`defworld--5` back to a single
  variadic-over-comps form.
- A new in-tree turmeric fixture mirroring the minimal repro pins the
  regression.

## Cross-references

- Surfaced during the E2d-P1+P5 ECS migration session (reverted before
  shipping; bug filed instead so a future session has the gap visible).
- Related: variadic HKT rows landed in turmeric 0.20.0 for value-level
  splices but type-position parsing wasn't updated in step.
