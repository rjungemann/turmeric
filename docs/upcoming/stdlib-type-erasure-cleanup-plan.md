# Stdlib Type-Erasure & Stub-Typeclass Cleanup Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-02
> **Type:** stdlib hygiene -- replace `int64_t`-erased typeclass stubs with real instances
> **Sibling plans:**
> - [stdlib-inline-c-deworkaround-plan.md](stdlib-inline-c-deworkaround-plan.md) -- replace inline-C workarounds
> - [stdlib-advanced-typing-plan.md](stdlib-advanced-typing-plan.md) -- linearity / sessions / effects / refinement / typeclasses
> - [stdlib-opaque-handle-types-plan.md](stdlib-opaque-handle-types-plan.md) -- handle nominal typing

---

## Overview

`stdlib/arrow.tur` documents a recurring failure mode: a typeclass surface is
declared (`Arrow`, `ArrowChoice`, `ArrowLoop`, ...), but no instances exist
because (a) required language features are missing (sum types, full HKT,
closure-capturing handlers), (b) codegen bugs corrupt closure-returning
instance methods, or (c) C name-mangling collisions prevent two combinators
from coexisting. The user-facing API is a set of free functions that pass
everything through `int64_t` and dispatch via hand-rolled inline-C
trampolines.

The same pattern appears in several other stdlib modules. This plan groups
them by root cause, lists the concrete blockers, and proposes the order in
which to fix them.

## Affected modules (by severity)

### Tier 1 -- typeclass declared, instances blocked by a single missing feature

| Module | Blocker | What unlocks the fix |
|---|---|---|
| `stdlib/arrow.tur` | `Either`/`Left`/`Right` sum types; closure-returning instance method codegen bug; `<<<`/`>>>` mangle to same C identifier | Sum-type lowering + dict-field type fix + identifier mangling for operator names |
| `stdlib/effects.tur:12` | Handler bodies cannot capture outer-scope variables (no closure analysis for handler `fn`s) | Closure analysis pass over effect-handler bodies |
| `stdlib/rc.tur:26` | `Functor [rc]` `fmap` cannot reach the wrapped value through `RcControlBlock` | Restructure `RcControlBlock` to expose the value, or a `with-rc-value` primitive |
| `stdlib/equal.tur:27` | `equal-cong` is gated on HKT | HKT phase already landing; just wire `equal-cong` once `^f` over user types is solid |
| `stdlib/schema.tur:580` | `Monad [Schema]` deliberately omitted (lawful `>>=` contradicts accumulating `<*>`) | Out of scope -- documented design choice, not a fix-target. Leave as-is. |

### Tier 2 -- `int64_t` type-erased dispatch with hand-rolled inline-C

These are not stubs -- they work -- but they have the same "everything is
`int64_t`, dispatch via inline-C trampoline" shape as `__arrow_call1/2`. They
should migrate to whatever real fat-closure dispatch mechanism the compiler
adopts (see the recent `httpd` fat-closure work, commit `bfab8c4c`).

| Module | Erasure point |
|---|---|
| `stdlib/logic.tur:17` | `Goal = UState -> list UState`; `UState` is a raw `int64_t` substitution pointer |
| `stdlib/select.tur` (+ sequence iterators) | `seq-call-fn0/1/2` trampolines call fat closures stored as `int64_t` |
| `stdlib/threadpool.tur:231` | `ThreadPoolTask` packed as `int64_t` with hand-written C layout |
| `stdlib/arrow.tur:84,89` | `__arrow_call1/2` trampolines (will fall out once Tier-1 instances exist) |

### Tier 3 -- codegen-bug-shaped limitations

The same compiler bugs that block arrow's instances surface elsewhere:

| Module | Symptom |
|---|---|
| `stdlib/httpd.tur:1688` | `(mw-cors opts)` curried partial application: `__pap` wrapper stores `CorsOpts` (struct) into an `int64_t` env field; C compile fails. Sibling of arrow's "dict field resolved to `void *` not `int64_t`" bug. |
| `stdlib/map.tur:392` | Key-typed wrappers are macros, so they cannot be passed as first-class function values; `map-new` cannot infer K/V from a nullary call. |

---

## Plan

### Phase A -- compiler prerequisites

Nothing in stdlib can be cleanly fixed until these land. Each is a separate
plan; this document just enumerates the dependency edges.

1. **Sum types (`Either`, `Left`, `Right`) end-to-end** -- unblocks
   `ArrowChoice` (`left`, `right`, `+++`, `|||`) and tightens many
   error-handling sites currently using two-of-tuple workarounds.
2. **Closure-returning instance method codegen** -- the "dict field resolved
   as `void *` instead of `int64_t`" bug. Fixing it unblocks every Tier-1
   typeclass instance for arrow, and the `mw-cors` currying case in httpd.
3. **Operator-name C identifier mangling** -- `>>>` and `<<<` both mangle to
   `___`. Needed for `Category` (both compositions) and for any future
   typeclass that wants symmetric operator pairs.
4. **Effect-handler closure capture** -- a closure analysis pass over the
   bodies of `fn`s passed to effect handlers. Likely shares machinery with
   the existing fat-closure work.
5. **Struct-in-`__pap` env codegen** -- store struct values in `__pap` env
   fields without truncating to `int64_t` (the `CorsOpts` case). Likely a
   sibling of #2.

### Phase B -- per-module follow-throughs (one PR each, gated on Phase A)

Each of these is a small, self-contained PR once its prerequisite lands.

- **B1. `arrow.tur`**: reintroduce `Arrow [->]`, `ArrowChoice [->]`,
  `ArrowZero [->]`, `ArrowPlus [->]`, `ArrowLoop [->]`, `ArrowApply [->]`.
  Delete `__arrow_call1/2` and the `__arrow_pair_*` inline-C helpers; let the
  fat-closure dispatch and `Tuple2` constructors do the work. Restore `<<<`
  once mangling is fixed. Gated on A1, A2, A3.
- **B2. `effects.tur`**: drop the "handlers cannot capture" caveat and
  rewrite the test suite to exercise capturing handlers. Gated on A4.
- **B3. `rc.tur`**: real `Functor [rc]` `fmap` against the restructured
  `RcControlBlock`. Gated on the RC restructuring (its own plan).
- **B4. `equal.tur`**: enable `equal-cong` once HKT is stable. Tiny diff;
  mostly removing the gating comment and adding fixtures.
- **B5. `httpd.tur`**: re-enable `(mw-cors opts)` curried partial
  application, remove the workaround comment at line 1688. Gated on A5.
- **B6. `map.tur`**: expose the key-checking wrappers as real functions (not
  macros) once the type checker can carry the constraint through a normal
  `defn`. Gated on a typeclass-constraint-on-defn task (separate plan).

### Phase C -- Tier-2 erasure migration (opportunistic)

Once `arrow.tur` is migrated off `__arrow_call*` in Phase B1, the same
fat-closure dispatch is available for:

- `stdlib/logic.tur` -- type `Goal` as a real arrow type, drop the
  `int64_t`-erased `UState` pointer.
- `stdlib/select.tur` and sequence iterators -- replace `seq-call-fn*`
  trampolines with the same dispatch.
- `stdlib/threadpool.tur` -- replace the hand-rolled `ThreadPoolTask`
  layout with a real task struct + typed closure.

These do not block anything user-facing; do them when touching the modules
for other reasons.

---

## Explicit non-goals

- **`schema.tur` Monad**: omission is by design (Applicative is accumulating,
  Monad would be fail-fast). Not a target. Document it in the schema guide
  instead.
- **Rewriting fat closures from scratch**: this plan assumes the existing
  fat-closure machinery (per commit `bfab8c4c`) is the destination, not a
  greenfield design.
- **Eliminating all `int64_t` in stdlib**: type-erased carriers are fine
  where the type checker still enforces identity at the call boundary (e.g.
  `Vec[A]`, `Option[A]`, `Map[K V]`). The target is the *dispatch*
  trampolines, not every `int64_t` field.

---

## Open questions

1. Is operator-name mangling (A3) worth doing standalone, or should it land
   with a broader symbol-mangling overhaul?
2. Does fixing A2 (closure-returning instance methods) also fix A5
   (struct-in-`__pap` env), or are they distinct codepaths in codegen?
3. For B6 (`map.tur`), is the right answer "real `defn` with constraint"
   or "leave as macro and document the limitation"?

## References

- `stdlib/arrow.tur:92-102` -- the canonical "instances intentionally
  omitted" comment that motivated this plan.
- Commit `bfab8c4c` -- recent fat-closure dispatch fix in httpd; likely the
  template for A2.
- [stdlib-inline-c-deworkaround-plan.md](stdlib-inline-c-deworkaround-plan.md)
  -- overlapping scope on the inline-C side.
- [stdlib-advanced-typing-plan.md](stdlib-advanced-typing-plan.md) -- the
  HKT/typeclass machinery this plan depends on.
