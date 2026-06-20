---
title: `list` macro expands to int-headed `tcons`, not the polymorphic `tcons-of`, so there is no float-list (or any non-int-headed) list literal
category: List literal ergonomics / typed-list surface -- expressiveness hole (not a miscompile)
severity: Medium. Not a miscompile -- it is a real expressiveness gap. The
  convenient `(list ...)` literal can only ever build an int-headed list, so any
  spice that wants a `(List float)` / `(Cons float)` must hand-write a
  `(tcons-of ...)` chain and carry the ascription by hand. This is exactly the
  friction the linalg / Track-C migration hits when expressing float vectors and
  matrices as lists. The diagnostic also mis-points (suggests tupleN, which is
  for heterogeneous fixed-arity data, when the user wants a homogeneous float
  list).
status: RESOLVED
---

## Resolution

Fix direction 1 (+ 3) landed: the `list` macro is now element-type-polymorphic.

- `stdlib/list.tur`: `list` no longer right-folds into the int-headed `tcons`.
  It right-folds into the polymorphic `tcons-of` (`list-build__`) and runs a
  pairwise homogeneity check (`tur-list-homog__` / `list-homog-chain__`,
  mirroring `vec-of`). So `(list 7.1 2.5)` builds a `(Cons float)` with a real
  `double` head slot; `(list 1 2 3)` stays a `(Cons int)`; an empty `(list)` is
  the bare `:int` carrier `0`. A `(Cons A)` result coerces to `:int` in argument
  position, so `list-length`, `list-eq?`, and the rest of the carrier API keep
  working unchanged. `list*` was likewise routed through `tcons-of` so a float
  prefix specializes its head.
- `src/compiler/elab_call.c`: the tupleN hint now fires on the homogeneity
  helper `tur-list-homog__` too. With the macro polymorphic, the only remaining
  `(list ...)` error case is a genuinely heterogeneous literal (e.g.
  `(list 1 "x" 3.14)`) -- which is exactly when tupleN is the right suggestion,
  so the secondary "diagnostic mis-points" gap closes naturally.
- Fixture `tests/fixtures/typed/list-macro-float` locks in float-list literals,
  float `thead`, float `list*` prepend, and int-list/empty-list compatibility.

This retires the linalg push-loop "Workaround #4": `la-vec-of` / `mat-of` can
lower a float literal through `(list ...)`. Note `stdlib/list-typed.tur` already
shipped the typed sibling `list-of` (fix direction 2); both paths now exist.

# `list` macro is wired to int-headed `tcons`, so float-list literals are impossible

## One-line summary

There *is* a polymorphic-head constructor, `tcons-of` (`stdlib/list.tur:55`),
which accepts a typed head (`A = float` gives a `double` head slot). But the
`list` macro (`stdlib/list.tur:217`) expands to plain `tcons`
(`stdlib/list.tur:30`), whose head parameter is `:int`. So the convenient
`(list ...)` literal only ever builds an int-headed list; there is no
float-list literal sugar. You have to hand-write
`(tcons-of 7.1 (tcons-of 2.5 (tnil)))` and carry it as `(Cons float)` /
`(List float)` yourself.

## Minimal repro

```turmeric
;; Float-list literal -- FAILS:
(defn main [] : int
  (let [xs (list 7.1 2.5)]
    (println (list-length xs))))
;; error [TUR-E0001]: function 'tcons' arg 1: expected int, got float
;; help: for heterogeneous fixed-arity collections, consider tuple2 ... instead

;; Hand-written tcons-of chain -- WORKS, prints 2:
(defn main [] : int
  (let [xs (tcons-of 7.1 (tcons-of 2.5 (tnil)))]
    (println (list-length (:: xs :int)))))
```

## Root cause

- `tcons` (`stdlib/list.tur:30`) is the int-carrier constructor:
  `(defn tcons [h : int t : int] : int ...)`. Its head slot is `:int`.
- `tcons-of` (`stdlib/list.tur:55`) is the polymorphic one:
  `(defn tcons-of [A] [h :A t :int] : (Cons A) ...)` -- a `float` head lays out
  as a `double` field with no user bit-cast.
- The `list` macro (`stdlib/list.tur:217`) right-folds into `tcons`, never
  `tcons-of`:
  `(defmacro list [& xs] (if (empty? xs) (list tnil) (list tcons (first xs) (list list ~@(rest xs)))))`

So the element type of every `(list ...)` literal is pinned to `:int` at the
constructor, independent of the literal's actual elements.

## Secondary observation -- the diagnostic mis-points

The `TUR-E0001` on `(list 7.1 ...)` suggests `tuple2/tuple3/...`. That hint is
for *heterogeneous fixed-arity* data; here the user wants a *homogeneous* float
list. The hint sends them away from the actual fix (a typed list) toward
tuples, which is the wrong shape.

## Fix directions (pick one)

1. **Make `list` element-type-polymorphic.** Expand to `tcons-of` instead of
   `tcons` so `(list 7.1 2.5)` yields a `(List float)` directly. `tcons-of`
   already exists and produces a typed `(Cons A)`; the macro change is the main
   work. Watch: the docstring on `list` (`stdlib/list.tur:190`) advertises the
   `:int` carrier result and downstream `:int`-taking ops (`list-length`,
   `list-eq?`, ...) -- those would need to accept `(List A)` or the macro would
   need to keep producing a carrier-compatible value. This is the real design
   question: unify the carrier `list-*` API with the typed `(Cons A)` one (see
   the companion finding on the half-present list API surface).
2. **Add a typed sibling literal** (e.g. `list-of` / `(list :float ...)`) that
   routes through `tcons-of`, leaving `list` as the int-carrier fast path. Less
   disruptive but grows the surface.
3. **At minimum, fix the diagnostic.** When `(list ...)` fails because the
   elements are a single non-int type, point at the typed path
   (`tcons-of` / `(List A)`), not at tupleN. The tupleN hint should be reserved
   for genuinely heterogeneous element types.

## Scope note

Compiler/stdlib-side (the `list` macro + the `tcons`/`tcons-of` split, plus the
TUR-E0001 hint text). Verified on turmeric built from source at branch
`claude/list-api-v0-21-0-d0wqdz` (post-v0.21.0). Closely related to the
half-present list-API-surface finding (`cons` builtin survives but the typed
and carrier accessor families don't line up): both stem from the carrier-`int`
vs typed-`(Cons A)` list duality not being reconciled.
