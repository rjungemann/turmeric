# `rc<int>` (angle-bracket) type annotation silently becomes a tyvar

**Status: RESOLVED (2026-07-20).** Fixed via the report's option (b) -- the
reference-family angle-bracket forms are now *parsed as the parameterised types
they look like* in defn/fn param and return annotations, instead of being
rejected or left to accidental unification. Option (a) (blanket-reject
`Ctor<...>`) was rejected: `rc<int>`, `ref<int>`, `lref<int>`, `weak<int>`, and
`ptr<T>` are used pervasively as param/return/field annotations across the
fixtures, so rejecting the spelling would have broken working code.

Resolution summary:

- New `rc_family_type_from_keyword_name` (`src/compiler/elab_types.c`), a sibling
  of `ptr_type_from_keyword_name`, resolves `rc<T>` / `weak<T>` / `ref<T>` /
  `lref<T>` to the real `TY_RC` / `TY_WEAK` / `TY_REF` / `TY_LREF` carrier
  (with `type_rc_adt` for an aggregate inner, mirroring the struct-field path).
- Wired into every annotation position that previously fell through to a tyvar:
  the fused `:rc<T>` keyword ladder in both `elab_defn` and `elab_fn` (param +
  return), and the spaced `: rc<T>` / nested path via `fn_type_from_form_impl`.
  `:ref<T>` / `:lref<T>` params inherit the linear-by-default discipline the
  bare-keyword forms apply.
- Regression fixture: `tests/fixtures/rc-typed-param-annotation/`.
- Full suite green (2213 passed, 0 failed); `rc/strong-count` on a
  `rc<int>`-typed param now reports the real count (verified end-to-end: 2 after
  a clone).

Original report follows.

---

**Severity: medium** -- it is a silent footgun: the annotation looks like it
names `rc<int>` but resolves to an unrelated fresh type variable, so rc builtins
fail with a confusing `got tyvar` and the value only behaves as an rc by
accidental unification.

## One-line summary

Angle-bracket generic syntax is NOT general type-application syntax in the type
parser. Only a few whole tokens are special-cased (e.g. `ptr<void>` at
`elab_core.c:32`). An unlisted `rc<int>` is read as a single opaque
type-variable *name* `"rc<int>"`, not `rc` applied to `int`. The canonical rc
annotation is the bare constructor `rc` (`elab_core.c:39`, `if (name == "rc")
return TY_RC`).

## Minimal repro

```turmeric
(defn f [r : rc<int>] : int (rc/strong-count r))   ; error: ... got tyvar
(defn g [r : rc]      : int (rc/strong-count r))   ; OK -- bare `rc` is TY_RC
```

`(defn h [] : rc<int> ...)` in RETURN position makes it explicit:
`unsupported return type keyword 'rc<int>': ... to use it as a type variable`.

## Why it is a silent footgun

A `rc<int>` param unifies with a real `rc` value under expected-type pressure
(a struct field of type `rc`, a call argument to a fn declaring `rc`), so
`(make-struct Own :r r)` and `(sink r)` "work" and the value behaves as an rc.
But a context with no such pressure -- any `rc/...` builtin, which post-hoc
checks `inner->type.kind == TY_RC` -- sees the bare tyvar and errors. So the
same binding is "rc enough" in one position and "a tyvar" in the next.

## Fix directions

Either (a) reject `Ctor<...>` angle-bracket forms in annotation position with a
diagnostic pointing at the bare-constructor / `(Ctor arg)` spelling, so the
mis-syntax is a hard error instead of a silent tyvar; or (b) parse `rc<T>` as the
type application it looks like. Option (a) is smaller and removes the footgun;
the codebase already uses bare `rc` where it matters.

## Note

This was initially (and wrongly) filed as an "rc parameter generalizes to a
tyvar" blocker for E3a (owning-cloneable-capture). It is not a generalization
defect -- bare `rc` params are concrete `TY_RC`. E3a's borrow-capture channel
landed using bare `rc`. The remaining E3a scope limit (an owning value the
*enclosing fn owns* and must drop after a cloneable-reset) is a separate
concern, tracked in the E3 plan as the E3b owning-autodrop-crossing case.
