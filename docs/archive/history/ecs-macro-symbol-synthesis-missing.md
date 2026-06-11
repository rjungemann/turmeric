---
title: Compile-time macros cannot synthesize identifier symbols
category: Reported
severity: Ergonomics gap (blocks `defworld` from emitting per-component named accessors)
discovered: 2026-06-10, during ECS spice E0 execution (docs/upcoming/ecs-spice-plan.md)
resolved: 2026-06-11, `str->sym` builtin landed in `src/compiler/elab_macros.c:321-326`
  (`ct_eval_builtin` switch arm). Implementation mirrors `dot-sym`: takes a
  string literal at expansion time, interns it via `symtab_intern`, returns a
  fresh F_SYM form. Verified end-to-end: `(defmacro mint-get [T] ...
  (str->sym (str-append "get-" (symbol-name T))) ...)` mints `get-Pos`,
  `get-Vel` etc. as top-level defns.
---

# Compile-time macros cannot synthesize identifier symbols

> **Status: fixed.** `str->sym` is shipped in
> `src/compiler/elab_macros.c:321-326`. The minimal repro from this
> report works as written. The ECS prerequisite plan
> ([`../upcoming/ecs-prereq-plan.md`](../upcoming/ecs-prereq-plan.md))
> Tier 1 is closed; the next blocker is gap D
> ([`macro-cannot-emit-inline-c-block.md`](macro-cannot-emit-inline-c-block.md)).

## Summary

The compile-time macro evaluator (`src/compiler/elab_macros.c`) exposes
`symbol-name` (Symbol -> String) and `dot-sym` (Symbol -> ".<symbol>"
Symbol) but has no general path from a *runtime-computed string* back to a
fresh Symbol. As a result, a macro cannot mint identifier names like
`set-Pos!` from the parts `set-`, `Pos`, `!`. This blocks the natural
shape of the ECS plan's `defworld` macro:

> Generates a `defstruct GameWorld` ... and per-component `get-Pos`,
> `set-Pos`, `remove-Pos`, ... accessors. (See
> `docs/upcoming/ecs-spice-plan.md` § "World".)

## Severity

Ergonomics. The macro substrate is otherwise complete -- backquote/unquote,
nested templates, struct field-name reflection, and recursive helpers all
work -- so this is a narrow but blocking gap whenever a macro wants to
emit a *family* of definitions keyed by a passed-in symbol.

In the E0 spice this forces the user-facing surface to be polymorphic
`dense-set!` / `dense-get` over a typed storage field rather than the
plan's `set-Pos! w e p` / `get-Pos w e`. The structural-typing win (field
access for component lookup) survives -- `(.Pos w)` still works because
the field name *is* the component name -- but the convenience aliases
the plan calls for cannot be emitted.

## Minimal repro

```turmeric
(defmacro mint-setter [TypeName]
  ;; want: emit a defn whose name is `set-<TypeName>!`
  (let [base   (symbol-name TypeName)            ;; -> "Pos"
        nm-str (str-append "set-" base "!")]     ;; -> "set-Pos!"
    `(defn ~(str->sym nm-str) [w v]              ;; <-- str->sym does not exist
       ...)))
```

Available compile-time builtins per `src/compiler/elab_macros.c`:

- `first` `rest` `second` `nil?` `empty?` `list?` `vec?`
- `symbol-name` (Symbol -> String)
- `dot-sym` (Symbol -> ".<sym>" Symbol)  -- the only Symbol-minting builtin
- `str-append` (Strings -> String)
- `cons` `list` `vec` `=` `not`

There is no `str->sym`, `symbol-concat`, `gensym`, or general
`intern-string-as-symbol`. `dot-sym` is the lone existing escape hatch
and only prepends a dot.

## Observed vs. expected

Observed: a macro can compute the string `"set-Pos!"` at expansion time
but has no way to use it as the binding name of a `(defn ...)` form.

Expected: a builtin `str->sym : String -> Symbol` (or a generalisation
of `dot-sym` to take a prefix/suffix) so a macro can mint identifier
names. Hygiene is not an issue here -- the macro author *wants* the
resulting symbol to be visible at the call site under a predictable
name.

## Root-cause pointer

`src/compiler/elab_macros.c:303-314` -- `dot-sym` already shows the
exact intern path (`symtab_intern(env->elab->st, strslice(buf, len))`).
A `str->sym` builtin would follow the same pattern: accept an `F_STR`,
intern its contents, return `form_sym`. Estimated implementation size:
~20 lines plus the `form_contains_ct_builtins` switch entry around
line 208.

## Proposed fix directions

1. **Smallest:** add `str->sym` mirroring `dot-sym`. Macros do their own
   string-building with `str-append` and then convert.
2. **More expressive:** add `sym-concat` (Symbol ... -> Symbol) as a
   convenience that does both steps in one shot.
3. **Optional:** add `gensym` for hygienic temporaries inside macro
   bodies. Independent of (1); not required to unblock the ECS use case.

Either (1) or (2) unblocks the ECS plan's `defworld` accessor generation
and the analogous per-component naming in other spices (effects,
notebooks, etc.).

## Related blocker discovered in the same session

Storing user-defined `defstruct` values in a generic `[A]`-parameterized
inline-C `defn` (the `dense-set!`/`dense-get` shape used by ECS) fails
at C compile time: the generated function signature monomorphises `A` to
`int64_t`, but the call site passes the struct *by value* (a `struct Pos`
literal), producing `passing 'Pos' to parameter of incompatible type
'int64_t'`. `vec.tur` sidesteps this by only ever storing int-carried
values (raw ints or pointer-backed opaque handles).

For ECS this means a multi-field component cannot be stored directly in
dense storage today; the workaround is to wrap the component in an
opaque heap handle (`pos-new x y -> int`, `pos-x : int -> int`, ...).
A proper fix would either:

- Have the generic codegen path use the actual monomorphised type in
  the C signature (treat `:A` like a real type variable rather than
  always lowering to `int64_t`), or
- Document this as a deliberate "generic inline-C is int-carrier only"
  restriction and provide a `box`/`unbox` blessed pattern.

This is filed as a separate report under
[generic-inline-c-struct-arg-monomorphises-to-int64.md](generic-inline-c-struct-arg-monomorphises-to-int64.md)
so the two issues can be triaged independently.
