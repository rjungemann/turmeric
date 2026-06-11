---
title: Generic `[A]` defn with inline-C body always lowers `A` to int64_t
category: Reported
severity: Expressiveness hole (blocks defstruct values in any generic container)
discovered: 2026-06-10, during ECS spice E0 execution (docs/upcoming/ecs-spice-plan.md)
resolved: 2026-06-10, see `src/compiler/emit_module.c` per-instantiation
  monomorphization gate (search `Per-instantiation monomorphization`).
---

# Generic `[A]` defn with inline-C body always lowers `A` to int64_t

> **Status: fixed in this PR.** The compiler already had an
> `EmitAbiSpecialization` infrastructure (clone interning, mangled
> per-instantiation names, dedup by lowered C type, call-site dispatch).
> The missing piece was an opt-in gate at `src/compiler/emit_module.c`
> in `emit_abi_register_call` (and its fn-value twin in
> `emit_abi_scan_fn_values`) that bailed out for every inline-C body
> without `__TUR_TY_<NAME>__` markers, regardless of whether the
> substituted ABI escaped the int64 carrier. The fix narrows that bail:
> the carrier path stays for substitutions that round-trip through
> int64 (ints, opaques, pointers, type-apps -- the cases vec.tur
> currently relies on, kept byte-identical), and a fresh
> per-instantiation clone is interned the moment a substituted slot is
> a non-carrier-ABI by-value `TY_STRUCT`. The minimal repro and the
> dense-set! / read-`.x` ECS validation both compile and run. Full
> validation-plan items below are checked off in `## Validation
> outcome`.

## Summary

A `defn` with a type parameter `A` and an inline-C body emits a single
C function whose parameter typed `:A` is monomorphized to `int64_t`.
Call sites that try to pass a `defstruct` value (i.e. an actual `struct
Foo` whose layout is *not* `int64_t`) fail at C compile time with:

```
error: passing 'Foo' (aka 'struct Foo') to parameter of incompatible
       type 'int64_t' (aka 'long long')
```

`stdlib/vec.tur` works because its `:A` slot is always populated by
int-sized carriers (raw ints, pointer-backed opaque handles). The bug
surfaces as soon as a real multi-field struct value enters the picture.

## Severity

Expressiveness hole. The natural type-parameterized data structure for
ECS-style storage -- "I store values of type `A` keyed by index" -- is
exactly the case that breaks. Every workaround (heap-allocate every
component behind an opaque int handle, then box/unbox at the storage
boundary) pushes a memory-management burden onto every component author
and defeats the dense-iteration story that is the whole point of dense
storage.

## Minimal repro

```turmeric
(defstruct Pos [x : int y : int])

(defn take-A [A] [v : A] : nil
  ```c
  (void)v;
  ```)

(defn main [] : int
  (let [p (make-struct Pos 1 2)]
    (take-A p)
    0))
```

Compiles cleanly through elaboration. Fails at the C compile step:

```
error: passing 'Pos' (aka 'struct Pos') to parameter of incompatible
       type 'int64_t' (aka 'long long')
    take_hyA(p_891);
             ^~~~~
note: passing argument to parameter 'v' here
    static void take_hyA(int64_t v) { ... }
```

Adding `:copy` to the struct does not change the outcome -- the by-value
calling convention is exactly the case that exposes the int64 lowering.

## Observed vs. expected

Observed: The generic-defn codegen path emits one C function with
`int64_t` substituted for every `:A` parameter, regardless of what `A`
monomorphizes to at the call site. The struct call site emits its
argument as a struct literal, producing the type mismatch.

Expected: The generic-defn codegen path monomorphizes per instantiation,
emitting a fresh C function per distinct `A` at each call site so that
the parameter type in C matches the actual struct/opaque/primitive type
flowing in.

## Root-cause pointer

Codegen for `defn [A] ...` does not generate per-instantiation copies;
it generates a single C function with int64_t for the generic slot. The
call-site emit, by contrast, types the argument according to its actual
type. The two diverge for any `A` whose runtime carrier is not int.

(I have not yet located the exact codegen line that decides "use
int64_t here"; if the broader project structure makes that easy to
point at, please update this report with the file:line.)

## Why it surfaced in ECS

The ECS spice's dense-storage layer wants exactly this signature:

```turmeric
(defn dense-set! [A] [s : int idx : int val : A] : nil ```c ... ```)
```

so that storing a `Pos` in a `(.Pos w)` storage works without per-type
boilerplate. The smoke test (`spawn1k.tur`) had to fall back to storing
plain ints because `make-struct Pos i (* i 2)` produces a `struct Pos`
value and cannot reach the generic worker.

## Proposed fix: per-instantiation monomorphization

Emit a fresh C function per distinct `A` at each call site -- the same
strategy Rust and Swift use for generic functions. Concretely:

- At elaboration, when a `(defn [A] ...)` call site resolves `A` to a
  concrete type `T`, record `(defn-id, T)` as an instantiation request.
- At codegen, emit one C function per distinct `(defn-id, T)`, with
  every `:A` slot rewritten to the actual C lowering of `T` (struct
  type for structs, `int64_t` for ints/opaques/pointers, etc.).
- Mangle the emitted function name with the type identity
  (`take_hyA__Pos`, `take_hyA__int`) and route each call site to the
  matching instantiation.
- De-duplicate instantiations with identical C lowerings (two opaques
  both backed by `int64_t` share one emit) to keep object size in
  check.

This is the most thorough fix: it resolves the ECS use case, every
analogous "store T in a generic container" pattern that vec.tur has
been sidestepping, and the subtler case of generic inline-C wanting to
read struct fields (currently impossible because the C body sees
`int64_t`, not `struct Pos`). The cost is a larger object file when a
generic is called with many distinct `A`s, mitigated by the
deduplication step above; this is the cost Rust and Swift accept.

### Implementation sketch

1. **Instantiation collection.** Add a per-defn `instantiations`
   set populated during elaboration whenever a generic call site
   monomorphizes. Key by the lowered C type of `A` (not the surface
   type) so the dedup falls out naturally.
2. **C type rewriting.** Where the generic codegen currently writes
   `int64_t` for an `:A` parameter / return / local, substitute the
   actual C type emitted for the instantiation's `A`. Inline-C bodies
   already see `A` as an opaque token -- the substitution should be a
   simple text-level replacement of the parameter declaration plus a
   guarantee that the inline-C body refers to `val` (not raw bits) so
   it survives the type change.
3. **Call-site dispatch.** Replace the single C function name with the
   mangled per-instantiation name at each call site. Done at the same
   point the type-checker resolves the call's `A`.
4. **stdlib re-emit.** `stdlib/vec.tur`'s int-only call sites continue
   to emit a single instantiation (the int64 one), so the generated C
   for stdlib stays byte-identical aside from name mangling. Existing
   fixture snapshots will need a one-shot regen under the strict
   fixture-snapshot rule (CLAUDE.md).

### Why not the other directions

For the record, two narrower alternatives were considered and rejected:

- **Int-carrier restriction with `box`/`unbox`.** Cleaner to implement
  but leaves the ergonomic hole exactly where it hurts most (the very
  case the ECS plan calls out as the dense-storage win), and any future
  feature that wants per-instantiation behavior (e.g., reading struct
  fields from inline-C) hits the same wall.
- **`IntCarried` marker class.** Half-measure: keeps the int64 emit
  path but moves the diagnostic earlier. Still rejects struct
  components and still forces the heap-handle workaround.

Per-instantiation monomorphization subsumes both: an int-carried `A`
trivially produces one shared instantiation, and a struct-carried `A`
produces a fresh one that Just Works.

## Validation plan

A fix is validated when:

- The minimal repro above compiles and runs.
- `dense-set!` / `dense-get` in `tur-ecs` accept a `Pos` struct without
  the explicit handle-boxing dance; the E0 smoke test can store a
  `Pos {x y}` directly and sum `(.x p)`.
- `stdlib/vec.tur`'s existing call sites (all int-carried) emit
  identical C modulo function-name mangling, and fixture snapshots are
  regenerated in the same PR.
- Two distinct `A`s in the same program produce two distinct emitted
  functions; two `A`s with identical C lowerings (e.g., two opaque
  `:int` types) produce one shared emit.

Until then, the E0 smoke test stores ints; `Pos` lands in E1' alongside
the typed-row query work that has to grapple with the same lowering
question.

## Validation outcome (2026-06-10)

- Minimal repro: compiles and runs (`take_A__spec__void_Pos(Pos v)`
  emitted alongside the int64 carrier; call site routes to the clone).
- `dense-set!`-style generic accepting `Pos` by value: compiles and
  runs; a sibling `(defn dense-pos-x [v : Pos] : int ```c return v.x; ```)`
  reads `.x` directly out of the substituted struct.
- `stdlib/vec.tur` int-only call sites: unchanged. The narrowed gate
  (only force the inline-C spec when at least one substituted slot is a
  non-carrier-ABI `TY_STRUCT`) keeps int-carried `A` on the existing
  carrier emit, so no vec.tur clone is interned and the emitted C is
  byte-identical.
- Dedup: two `:A`s that lower to the same C type share one emit (the
  pre-existing `emit_abi_intern_spec` matches on the cloned `arg_types`
  by `type_eq`, so an opaque `:int` and a transparent newtype both
  carried as `int64_t` collapse to one clone). Two distinct `A`s (e.g.
  `Pos` vs `int`) produce two clones.
- Test suite: pre-change baseline had **204** failures (unrelated to
  this report -- mostly clang-strictness errors in inline-C bodies on
  this macOS toolchain); post-change suite has **166** failures.  The
  delta is a net reduction of **38**: every fixture that previously
  passed still passes, and 38 fixtures that fell into the old int64
  miscompile now succeed. No new regressions.

### Why the gate is narrow rather than universal

The first attempt removed the gate entirely. That cascaded broken
codegen into 24 additional fixtures whose inline-C bodies hand-roll
`int64_t` / `int64_t *` for `:A` slots (e.g. `bytes-alloc`'s
`int64_t *buf = malloc(...); return buf;`). Those bodies are written
to the *carrier* contract -- they assume the C signature has int64
slots and the call site bridges via `(int64_t)(intptr_t)`. Forcing a
spec where no slot actually escapes the carrier ABI emits a clone
whose substituted signature contradicts its hand-rolled body. The
narrowed rule -- "spec only when a substituted slot is a non-carrier
`TY_STRUCT`" -- is the precise predicate: the carrier path stays
correct for every body it was already correct for, and the new spec
path activates exactly when the carrier would otherwise miscompile.

A future cleanup is to migrate stdlib's hand-rolled int64 bodies to
the `__TUR_TY_<NAME>__` marker form so they auto-specialize across
the board; that is independent of this fix.
