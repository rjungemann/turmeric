# CT Macro Primitives Cannot Walk `F_TYPE_ANN` Nodes (No `type-ann-inner`)

> **Status:** Resolved 2026-06-25 -- `type-ann-inner` and
>   `type-ann?` landed as CT primitives in
>   `src/compiler/elab_macros.c::ct_eval_builtin` (entries also
>   added to `form_contains_ct_builtins` so a macro body referencing
>   them triggers CT eval). Regression fixture:
>   `tests/fixtures/macro-type-ann-walk/` (exercises both
>   primitives on `(name : type default)` and asserts `type-ann?`
>   is `false` on a plain symbol slot). Sibling-repo follow-up
>   landed alongside: `defgodot-export` in
>   `../turmeric-godot/src/bridge/prelude.cpp` reverted to the
>   plan's structural-colon shape `(name : type default)` via
>   `type-ann-inner`; `examples/spike/scripts/defgodot_script_richer.tur`
>   updated; headless-verified PASS against stock Godot 4.x
>   (macOS arm64).
> **Discovered:** 2026-06-25 while shipping the richer
>   `defgodot-export` / `defgodot-signal` macros in
>   `../turmeric-godot/src/bridge/prelude.cpp` (see
>   [godot-language-binding-plan.md](../upcoming/v1/godot-language-binding-plan.md)).
> **Severity:** Medium. Blocks the plan's preferred
>   `(name : type default)` macro surface for any DSL that wants to
>   read the type and the name out of an annotated form. Workable
>   today by dropping the colon (`(name type default)`), but the
>   surface no longer matches what `defn` / `defstruct` /
>   `defgadt` users already read from the language.
> **Related (closed):** Spun out from the same wave of macro-eval
>   gaps tracked in
>   [defgodot-script-macro-vec-quote-semantics.md](../archive/defgodot-script-macro-vec-quote-semantics.md)
>   (parent report). The 2026-06-25 fixes closed every other gap;
>   this one was only surfaced once the rest of the surface worked
>   end-to-end.

---

## Summary

A macro that takes `^syntax` args can already walk arbitrary
parenthesized DSL shapes -- `(first d)`, `(rest d)`, `(second d)`,
`(symbol-name x)` are all available as compile-time primitives in
`src/compiler/elab_macros.c::ct_eval_builtin`. **Except** when the
shape contains a structural type annotation written `name : type`.

The reader (`src/compiler/reader.c:472-488`) folds `: type` into a
single `F_TYPE_ANN(inner)` node, where `inner` is the type form
sitting in `items[0]` with `len = 1`. So:

```turmeric
;; surface
(label-tag : string "ok-from-export")

;; AST shape (3-element F_LIST)
;;   items[0] = F_SYM(label-tag)
;;   items[1] = F_TYPE_ANN(items[0]=F_SYM(string), len=1)
;;   items[2] = F_STR("ok-from-export")
```

The CT primitives only descend into `F_LIST` and `F_VEC`
(`elab_macros.c:337,342,350`). `F_TYPE_ANN` is rejected:

```c
if ((args[0]->tag != F_LIST && args[0]->tag != F_VEC) || ...)
    return ct_value_form(form_nil(...));   // silent nil
```

So a macro that calls `(first (rest d))` to read the type-name gets
back the `F_TYPE_ANN` *node*, and `(symbol-name ...)` then errors
because the node's tag isn't `F_SYM`:

```
error: compile-time symbol-name expects a symbol
```

There is no CT primitive that unwraps an `F_TYPE_ANN`. The macro
author has no way forward without dropping the colon from the
surface.

## Reproducer

```turmeric
(defmacro probe [^syntax d]
  ;; d shape: (name : type default)
  (let [name-sym (first d)
        type-sym (first (rest d))]
    `(do
       (println ~(symbol-name name-sym))
       (println ~(symbol-name type-sym)))))   ;; <-- fails here

(defn main [] : int
  (probe (label-tag : string "ok-from-export"))
  0)
```

```
$ ./build/tur run probe.tur
probe.tur:5:18: error: compile-time symbol-name expects a symbol
```

Without the colon -- `(probe (label-tag string "ok-from-export"))` --
the macro runs cleanly. The bug is the macro author has no way to
*see through* the annotation node.

## Root cause

`form_type_ann` is constructed in `src/compiler/forms.c:142-149`:

```c
Form *form_type_ann(Arena *a, Span span, Form *inner) {
    Form *f = form_new(a, F_TYPE_ANN, span);
    Form **items = (Form **)arena_alloc(a, sizeof(Form *));
    items[0] = inner;
    f->as.list.items = items;
    f->as.list.len = 1;
    return f;
}
```

The layout is identical to a one-element `F_LIST` (uses the same
`as.list` union) -- the only difference is the `tag`. The CT
walkers in `ct_eval_builtin` could already read `items[0]`; they
just refuse on tag grounds.

## Fix direction (recommended -- option 2)

Add a dedicated `type-ann-inner` CT primitive next to
`symbol-name` in `src/compiler/elab_macros.c::ct_eval_builtin`. The
implementation is one entry:

```c
if (ct_symbol_name(name, "type-ann-inner")) {
    if (n_args != 1) {
        *env->ok = false;
        diag_emit(DIAG_ERROR, span,
                  "compile-time type-ann-inner expects 1 argument");
        return ct_value_form(form_nil(env->elab->arena, span));
    }
    if (args[0]->tag != F_TYPE_ANN) {
        *env->ok = false;
        diag_emit(DIAG_ERROR, span,
                  "compile-time type-ann-inner expects an "
                  "annotation form");
        return ct_value_form(form_nil(env->elab->arena, span));
    }
    return ct_value_form(args[0]->as.list.items[0]);
}
```

Plus the matching entry in `form_contains_ct_builtins`
(`elab_macros.c:258-275`) so the walker triggers CT eval when the
macro body references it.

A companion `type-ann?` predicate is worth shipping alongside, for
the common `(if (type-ann? slot) (type-ann-inner slot) slot)`
guard pattern.

### Why option 2 (dedicated primitive) over option 1 (loosen `first`)

The cheaper-looking change is to teach `first`/`rest`/`second` to
descend through `F_TYPE_ANN` -- the layout already matches and
every annotated slot would Just Work. But that conflates two
distinct AST kinds in macro-author code:

- A macro that *wants* to distinguish "this slot was annotated"
  from "this slot is a plain list of one element" would have no
  way to tell them apart anymore.
- Annotations carry semantic intent (the user explicitly wrote
  `: type`); collapsing them into list-traversal hides that intent
  from any downstream macro that might want to lower an annotation
  differently from an un-annotated slot (e.g. emit a contract,
  emit a coercion, refuse, warn).
- `(list? x)` would lie -- it would still say `false` for
  `F_TYPE_ANN`, but `(first x)` would succeed, leaving the macro
  author with an inconsistent predicate set.

A dedicated primitive keeps the AST shapes honest. The DSLs we
care about (godot-export, future typed-spice macros, derive-*
helpers that want to read field types) all want the type as
*data*, not as an opaque payload, and benefit from being able to
ask "is this annotated?" as a first-class question.

## Caller impact (godot macros)

Once `type-ann-inner` lands, the
`defgodot-export` macro in
`../turmeric-godot/src/bridge/prelude.cpp` reverts to the plan's
preferred shape:

```turmeric
;; surface
(defgodot-export (label-tag : string "ok-from-export")
                 (counter   : int    7))

;; macro body
(defmacro defgodot-export [& ^syntax decls]
  (if (empty? decls)
    `(do)
    (let [d        (first decls)
          name-sym (first d)
          type-ann (first (rest d))                  ;; F_TYPE_ANN
          type-sym (type-ann-inner type-ann)         ;; <-- new
          default  (first (rest (rest d)))
          name-str (symbol-name name-sym)
          type-str (symbol-name type-sym)]
      `(do
         (godot-export ~name-str ~type-str ~default)
         (defgodot-export ~@(rest decls))))))
```

The spike fixture `examples/spike/scripts/defgodot_script_richer.{tur,gd}`
flips back to the colon shape verbatim from the plan, and the
shell-macro work in the godot binding plan can be marked
"plan-shape" rather than "approximate shape."

## Tasks

- [ ] Add `type-ann-inner` + `type-ann?` CT primitives in
      `src/compiler/elab_macros.c` (entries in `ct_eval_builtin`
      and `form_contains_ct_builtins`).
- [ ] Fixture: `tests/fixtures/macro-type-ann-walk/` exercising
      both primitives on `(name : type default)` shapes (mirror
      `tests/fixtures/macro-syntax-param-rest/`).
- [ ] Sibling-repo follow-up in `../turmeric-godot/`: swap the
      `defgodot-export` macro in
      `src/bridge/prelude.cpp` back to the colon shape using
      `type-ann-inner`; update
      `examples/spike/scripts/defgodot_script_richer.tur` to the
      `(name : type default)` surface; re-run the headless
      `defgodot_script_richer.gd` PASS.
- [ ] Plan refresh: update the G7 status row in
      [godot-language-binding-plan.md](../upcoming/v1/godot-language-binding-plan.md)
      to "shell + ergonomic sub-macros (plan-shape)" and remove
      the "shape note" deferral.
