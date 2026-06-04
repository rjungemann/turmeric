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

Nothing in stdlib can be cleanly fixed until these land. Each sub-phase is a
separate plan; this document enumerates the dependency edges and the concrete
tasks each sub-phase contains.

#### A1. Sum types (`Either`, `Left`, `Right`) end-to-end

Unblocks `ArrowChoice` (`left`, `right`, `+++`, `|||`) and tightens many
error-handling sites currently using two-of-tuple workarounds.

1. Design the tagged-union memory layout for binary sums (`Either L R`) --
   reuse the `Option`/`Result` discriminant convention.
2. Add parser/AST support for `defdata`-style sum declarations (or extend the
   existing ADT surface) covering nullary and unary constructors.
3. Implement constructor lowering: `Left x` / `Right y` to a tagged struct
   literal, with pattern-match destructuring in `case`.
4. Implement exhaustiveness checking for `Either` matches; emit a warning on
   non-exhaustive patterns.
5. Wire `Either` into the typeclass instance resolver so `Functor`,
   `Applicative`, `Monad` instances can be written by hand against it.
6. Add fixtures: `tests/fixtures/sum-either-basic/`, `.../sum-either-match/`,
   `.../sum-either-nested/` and corresponding `expected.c` snapshots.
7. Migrate one existing two-of-tuple error site in stdlib to `Either` as a
   smoke test before B1 starts.

#### A2. Closure-returning instance method codegen

> **Superseded for execution by
> [closure-returning-instance-method-codegen-plan](closure-returning-instance-method-codegen-plan.md)
> -- LANDED 2026-06-03.** Closure-returning `definstance` methods now carry the
> `int64_t` fat-closure handle through the dict field, the method impl
> signature, and the call-result let-binding; curried (closure-returning-closure)
> returns are covered too. Coverage lives in
> `tests/fixtures/instance-closure-return-*`. The `mw-cors` struct-in-`__pap`-env
> case (A5) was confirmed a **separate** root cause and is left standing. This
> subsection remains as a coordinating index.

The "dict field resolved as `void *` instead of `int64_t`" bug. Fixing it
unblocks every Tier-1 typeclass instance for arrow, and the `mw-cors`
currying case in httpd.

1. Reproduce the bug in a minimal fixture: a `definstance` whose method
   returns a closure capturing a free variable.
2. Locate the dict-field type resolution site in the typeclass lowering pass;
   identify why the return type erases to `void *`.
3. Fix the type propagation so closure-returning dict fields carry their
   `int64_t` (fat-closure handle) type through to C emission.
4. Audit other dict field types (struct returns, opaque returns) for the same
   bug class; document findings as a note for A5.
5. Add fixtures covering: closure return, nested closure return, closure
   capturing a struct, closure returning another instance method.
6. Regenerate affected `expected.c` snapshots and confirm `bash tests/run.sh`
   is clean.

#### A3. Operator-name C identifier mangling -- LANDED 2026-06-04

> **Landed.** Binding-name mangling now routes through a single shared helper
> (`src/compiler/mangle.{h,c}`, `tur_mangle_append`) used by both
> `elab_mangle_binding_name` and the emitter's `raw_name_for_binding` /
> `c_name_for_binding`, so a function's declaration, definition, and call sites
> always agree. Each operator/sigil character gets a distinct two-letter
> mnemonic (`>` -> `_gt`, `<` -> `_lt`, `?` -> `_qu`, `!` -> `_ex`, ...) plus a
> `_xHH` hex escape hatch, so `>>>` (`_gt_gt_gt`) and `<<<` (`_lt_lt_lt`) -- and
> any symmetric operator pair -- coexist. The structural separators `-` and `/`
> deliberately keep the legacy single-`_` spelling so the hundreds of existing
> kebab/namespaced names are not churned. Coverage:
> `tests/fixtures/operator-mangle-pair`.

`>>>` and `<<<` both mangle to `___`. Needed for `Category` (both
compositions) and for any future typeclass that wants symmetric operator
pairs.

1. ~~Inventory current operator-to-identifier mapping in the mangler.~~ Done:
   every non-`[A-Za-z0-9_]` char collapsed to a single `_`, so all sigil names
   (`?`, `!`, `=`, `<`, `>`, ...) were mutually colliding, not just `>>>`/`<<<`.
2. ~~Design a stable, reversible mangling scheme.~~ Done -- two-letter mnemonics
   + `_xHH` escape (see helper). `-`/`/` keep the legacy `_` (low-churn choice).
3. ~~Implement the new mangler behind a single helper.~~ Done -- `tur_mangle_append`.
4. **Demangler intentionally omitted.** Because `-`, `/`, and a literal `_` all
   fold to `_`, the encoding is not self-delimiting and no *sound* inverse
   exists without also re-encoding those separators (which would churn every
   identifier). It is also unnecessary: diagnostics report the original *source*
   symbol name, never the mangled C identifier, so users already see `>>>`.
   Rationale is documented in `mangle.h`. (Resolves open question #1: A3 landed
   standalone, scoped to operator/sigil names, no broader symbol overhaul.)
5. ~~Add a fixture defining both `>>>` and `<<<`.~~ Done --
   `tests/fixtures/operator-mangle-pair` defines `>>>`, `<<<`, `===`, `!!!`.
6. ~~Regenerate affected snapshots.~~ Done -- 115/116 codegen snapshots changed
   (every fixture inlines the stdlib preamble, which carries predicate/bang
   names); all sigil-named inline-C references in `stdlib/httpd.tur`,
   `stdlib/httpd-compress.tur`, `stdlib/map.tur`, and `stdlib/sym.tur` that
   hardcoded the old `_`-mangled spelling were updated to match.

#### A4. Effect-handler closure capture

A closure analysis pass over the bodies of `fn`s passed to effect handlers.
Likely shares machinery with the existing fat-closure work (commit
`bfab8c4c`).

1. Identify the AST node(s) where handler `fn`s are introduced; tag them so
   the closure-analysis pass can find them.
2. Extend the existing fat-closure free-variable analysis to recurse into
   handler bodies.
3. Allocate and thread the fat-closure env through the handler dispatch
   trampoline; ensure the env survives across the resume boundary.
4. Confirm interaction with `resume` / one-shot continuations -- a captured
   variable must not be freed before the continuation fires.
5. Add fixtures: handler capturing a loop counter, handler capturing a
   struct, nested handlers each capturing distinct vars.
6. Stress-test with the existing effects suite under ASan to catch
   use-after-free regressions.

#### A5. Struct-in-`__pap` env codegen

Store struct values in `__pap` env fields without truncating to `int64_t`
(the `CorsOpts` case). Likely a sibling of A2 -- see open question 2.

1. Reproduce the `mw-cors` failure in a minimal fixture: a curried function
   whose first argument is a struct.
2. Inspect the `__pap` env struct emission: confirm the field is being
   declared as `int64_t` when the captured value is a struct.
3. Extend the `__pap` env layout to carry the real struct type (by value or
   by boxed pointer -- decide based on size threshold).
4. Update the `__pap` apply trampoline to read the struct field with the
   correct type rather than reinterpreting an `int64_t`.
5. Decide (with A2 findings in hand) whether this is a separate codepath or
   the same fix; collapse if possible.
6. Add fixtures for: small struct capture, large struct capture, struct +
   primitive mixed capture.
7. Regenerate snapshots and verify the httpd suite passes once `mw-cors`
   currying is re-enabled (the B5 smoke test).

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

1. ~~Is operator-name mangling (A3) worth doing standalone, or should it land
   with a broader symbol-mangling overhaul?~~ **Resolved (2026-06-04):** landed
   standalone. Scoped to operator/sigil names via one shared helper; structural
   separators (`-`/`/`) kept their legacy `_` spelling to bound the churn.
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
