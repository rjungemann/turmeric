---
title: defopaque [A] with struct A payload fails through the (unsafe (__raw)) lifting pattern that sized-storage relies on
severity: medium -- blocks lifting parametric storages (ecs/storage Dense, ecs/sparse Sparse) when component type A is a by-value struct; the int-A and opaque-A cases work, so the issue is invisible until a real defstruct component flows through. Class of bug appears related to M3 carrier-bridge / generic-inline-c-struct-arg monomorphisation.
status: RESOLVED 2026-06-17. Two distinct compiler bugs combined here; both
  are fixed and a regression fixture pins the result.  See the resolution
  banner below.
discovered: 2026-06-16
surfaced-by: E2d-P1+P5 migration in turmeric-spices (the spawn1k-pos and defcomponent-accessors fixtures regressed when (Dense A) accessors were threaded against A=Pos with Pos a `{x:int, y:int}` defstruct)
---

> **RESOLVED 2026-06-17.** The exemplar now compiles and round-trips a
> by-value struct through a parametric `(Dense A)` storage built from
> generic inline-C helpers (new / set! / get via `(unsafe ...)`).
> Regression fixture: `tests/fixtures/generic-inline-c-struct-through-unsafe/`.
> Full suite green (`1660 passed, 0 failed`); interpreter harness green
> (`1215 passed`).
>
> Two independent bugs were responsible:
>
> 1. **Return-only-polymorphic inline-C helper never specialized.** A helper
>    whose result is a bare type variable carried ONLY in the return
>    position (e.g. `(defn __dense-get [A] [d : int idx : int] : A ...)`) got
>    no argument-derived `abi_bindings`.  Inside an enclosing generic body
>    the expected return is that body's own (non-ground) tyvar, so the
>    ground-only return-binding branch in `elab_call.c` declined to bind, and
>    the call reached emit with zero bindings -> `emit_abi_register_call`
>    early-returned -> the helper was emitted once on the int64 carrier,
>    miscompiling the struct return.  Fix:
>    - `elab_fns.c`: push a BARE-tyvar return type onto the body's
>      expected-type channel (previously skipped) so a return-only-poly tail
>      call has the enclosing tyvar as a witness.
>    - `elab_call.c`: record the callee-result-tyvar -> caller-tyvar mapping
>      for such calls (gated to bare-tyvar results so the compound `(Map K V)`
>      relay case the existing guard protects is untouched).
>    - `emit_module.c`: recover the concrete by-value struct result by
>      instantiating the callee's result tyvar through the composed
>      specialization bindings (the elab side leaves `call->type` collapsed to
>      the int64 carrier), so the result ABI change is visible and a
>      per-instantiation clone is minted.
>
> 2. **Struct result truncated through the `unsafe` fiber slot.** `(unsafe
>    body)` desugars to an effect handler for the built-in `Unsafe` effect,
>    whose body result is routed through the fiber's `int64_t` result slot --
>    truncating a by-value struct return ("aggregate value used where an
>    integer was expected").  Unsafe is a pure compile-time marker that is
>    never actually performed, so the fiber-lift never suspends.  Fix: tag the
>    desugared handle with `HandleExpr.is_unsafe_marker` (`elab_unsafe.c`) and
>    emit its body directly in place -- as a value in expression position
>    (`emit_effects.c`, preserving the real C type) and as a statement in
>    statement position (`emit_stmt.c`, so side effects still run).
>
> The earlier mode-1/mode-2 framing (env-slot mismatch / spurious deref) was
> the same two root causes seen through the lens of two different workaround
> attempts; the spurious-deref-on-`(w).Pos` symptom is actually the separate
> nullary-field-reads-back-as-int bug
> (`defstruct-bare-user-type-field-reads-back-as-int-carrier.md`).

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
