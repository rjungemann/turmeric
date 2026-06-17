---
title: defopaque [A] with struct A payload fails through the (unsafe (__raw)) lifting pattern that sized-storage relies on
severity: medium -- blocks lifting parametric storages (ecs/storage Dense, ecs/sparse Sparse) when component type A is a by-value struct; the int-A and opaque-A cases work, so the issue is invisible until a real defstruct component flows through. Class of bug appears related to M3 carrier-bridge / generic-inline-c-struct-arg monomorphisation.
status: open
discovered: 2026-06-16
re-confirmed: 2026-06-17 -- during E2d triage this remained the live tracking
  item for the `(unsafe (__raw))` struct-A mis-typed-return path; it did not
  block the E2d landing (the 0-5 arity cascade and per-site re-pins worked
  around it) but the underlying carrier-bridge defect is unfixed. Keep open.
surfaced-by: E2d-P1+P5 migration in turmeric-spices (the spawn1k-pos and defcomponent-accessors fixtures regressed when (Dense A) accessors were threaded against A=Pos with Pos a `{x:int, y:int}` defstruct)
---

# `defopaque [A] :int` + struct-A payload broken through `(unsafe (__raw))` lift

## One-line summary

The exemplar pattern in `spices/ecs/src/ecs/sized-storage.tur`:

```turmeric
(defopaque SizedDense [n A] :int)
(defn sized-dense-new [n A] [k : int] : (SizedDense n A)
  (:: (unsafe (__sized-dense-new-raw k)) :SizedDense))
(defn sized-dense-set! [n A] [s : (SizedDense n A) idx : int val : A] : nil
  (unsafe (__sized-dense-set!-raw (:: s :int) idx val)))
```

works for `A = int`, `A = bool`, and `A = SomeOpaque [..] :int`, but
breaks at C compilation when `A` is a by-value struct like
`(defstruct Pos [x : int y : int])`. Two failure modes show up:

1. The `unsafe` block lifts the body into a closure with an env struct;
   the env slot for `val` stays `int64_t` instead of the specialized
   `Pos`, producing C errors like `assigning to 'int64_t' from
   incompatible type 'Pos'`.
2. Pivoting to an inline-C body that takes `A` directly produces a
   spurious pointer deref at the call site:
   `(*(int64_t *)(intptr_t)((w).Pos))` — treating the `(Dense Pos)`
   field as if it pointed at an int64 it could load.

Neither approach works for struct-A. The session that surfaced this
landed `spawn1k-pos` and `defcomponent-accessors` as new FAILs and was
reverted.

## The class of bug

Looks adjacent to:
- `docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`
- The archived `generic-inline-c-struct-arg-monomorphises-to-int64.md`
  (now under `docs/archive/history/`) -- whose comment on
  `spices/ecs/src/ecs/storage.tur:14-15` claims that fix landed and
  enabled struct components for the unparameterized storage handle.

The sized-* exemplar storage has no struct-A fixture, so the bug went
unnoticed there. As soon as the SAME lifted-public-fn-over-inline-C-raw
pattern is applied to the *unsized* `Dense [A]` and then exercised with
a real defstruct component, both monomorphisation paths fail.

## Why this matters

E2d-P1+P5 (ECS plan, lines 446-502 of `docs/upcoming/ecs-spice-plan.md`)
is blocked on this. The whole point of lifting `dense-new`/`dense-set!`/
`dense-get` to take `(Dense A)` is to thread the component type through
the world's field types -- but components are exactly the case where
`A` is a user-defined struct. The unsized exemplar has to work for
struct-A, not just int-A and opaque-A.

A fix here unblocks the E2d landing without needing a per-arity
`defworld` workaround.

## Minimal repro -- TODO

The session was reverted before a clean standalone repro was extracted.
A future investigation should reproduce both failure modes (the
`unsafe`-lift closure-env mismatch AND the inline-C spurious deref) in
small self-contained `.tur` files, then trace each to the responsible
emit-site.

The shape to start from:

```turmeric
(defopaque Box [A] :int)

(defn make-box [A] [] : (Box A)
  (:: (unsafe (__mk)) :Box))
(defn __mk [] #{Unsafe} : int ```c return 0; ```)

(defn box-set! [A] [b : (Box A) v : A] : nil
  (unsafe (__set (:: b :int) v)))
(defn __set [A] [b : int v : A] #{Unsafe} : nil
  ```c return; ```)

(defstruct Pos [x : int y : int])

(defn use-box [b : (Box Pos)] : nil
  (box-set! b (make-struct Pos 1 2)))
```

(My quick probe of this shape ran into a different elaboration error --
`TUR-E0002: function '__set' returns nil, which is not callable` -- so
the minimal-repro shape needs further work. The full failure was
reproducible in the spice fixtures
`spawn1k-pos.tur` and `defcomponent-accessors.tur` against the
mid-migration tree.)

## Proposed validation when fixed

1. The minimal repro (once cleaned up) compiles and runs without
   `-Wint-conversion` or "assigning to int64_t from incompatible type"
   errors.
2. The E2d-P1+P5 migration in turmeric-spices (replay the steps
   subagent took: lift storage.tur / sparse.tur / tag.tur to
   defopaques + update world.tur and query.tur macros + update test
   imports) lands with zero new spice-test failures.
3. `spawn1k-pos.tur` (struct-A component) and
   `defcomponent-accessors.tur` (typed get-/set-! over struct
   components) both pass.

## Cross-references

- `docs/upcoming/ecs-spice-plan.md` (E2d block, lines 446-502) -- the
  consumer plan blocked on this.
- `docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`
  -- adjacent class of bug.
- `docs/archive/history/generic-inline-c-struct-arg-monomorphises-to-int64.md`
  -- the archived fix that this report shows is incomplete for the
  defopaque+struct-A composition.
- `docs/reported/macro-template-type-position-rejects-unquoted-compound.md`
  -- companion report from the same session.
