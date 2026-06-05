# Stdlib Type-Erasure & Stub-Typeclass Cleanup Plan

> **Status:** Phase A landed; Phase B landed (B1 spun off, B6 still gated)
> **Last Updated:** 2026-06-05
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

> **Superseded by [[sum-types-either-plan]] -- see that plan for the detailed
> task list and the ADR of decisions taken.** This subsection is retained for
> context; the standalone plan is the source of truth. Note one deviation: A1
> step 4 below specced a *warning* on non-exhaustive matches, but the shipped
> behaviour keeps the pre-existing hard **error** and adds a `#{NonExhaustive}`
> opt-out marker instead.

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

#### A4. Effect-handler closure capture -- LANDED 2026-06-05

> **Landed.** The core capture machinery already existed: handler bodies and
> handle bodies are run through `collect_handle_captures`
> (`src/compiler/emit_core.c`), packed into a per-handle `__HEnv_<id>` env
> struct, and threaded through the dispatch trampoline via
> `TurEffectCaptureCtx::body_env` (`src/compiler/emit_effects.c`). Value,
> struct, loop-local, and `EX_CLOSURE`/`EX_DEFER` captures all worked. This
> phase closed the two remaining gaps the ASan sweep exposed:
>
> 1. **Nested-handle case-body captures were dropped.** The `EX_HANDLE` arm of
>    `collect_handle_captures` walked only the inner handle's *body*, not its
>    *case* bodies. But a nested handle's env-fill (`__henv_inner->f = f`) is
>    emitted in the *enclosing* function and references those names directly, so
>    a variable captured only by an inner case body (e.g. `b`, three frames out)
>    became an undeclared-identifier **hard C compile error**. Fix: the
>    `EX_HANDLE` arm now merges the inner handle's transitive case-body captures
>    (minus that inner case's own effect-params/`k`) into the current env --
>    mirroring the existing `EX_CLOSURE` handling. Shared via a new `cap_append`
>    helper.
> 2. **Use-after-free in the handle teardown.** The teardown freed the fiber,
>    then read `fiber->done` again to decide whether to free the capture env --
>    a heap-use-after-free that only "worked" because the freed slot was not yet
>    reused. Fix: cache `done` into a local up front and free the env before the
>    fiber (`src/compiler/emit_effects.c`).
>
> Interaction with `resume`/one-shot continuations is sound: the env lives on
> the heap (not the fiber stack) and is only freed once the fiber reports
> `done`; if `k` is stored (fiber not done) the env is intentionally retained.
> Coverage: `tests/fixtures/effect-handler-capture-loop`,
> `.../effect-handler-capture-struct`, `.../effect-handler-capture-nested`. All
> are ASan-clean (`-fsanitize=address,undefined`, `detect_leaks=1`).
>
> **Orthogonal finding (not fixed here):** any `(fn ...)` that captures a free
> variable heap-allocates a fat-closure env that is never freed, so a closure
> built inside a loop or a repeatedly-invoked handler leaks one env per
> construction. This is a general fat-closure lifetime gap, not effect-specific;
> reported in `docs/reported/fat-closure-env-leak.md`. The B2 follow-through
> (rewrite the effects test suite to exercise capturing handlers) should account
> for it.

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

#### A5. Struct-in-`__pap` env codegen -- LANDED 2026-06-04

> **Landed.** Two distinct codegen defects had to be fixed; they are
> **separate codepaths** from A2 (resolving open question #2 -- see below).
>
> 1. **`__pap` env field type erasure** (`src/compiler/elab_call.c`,
>    `elab_partial_apply`). A captured partial-application argument's binding
>    type was built from `type_from_kind(cap_kind)`, erasing a nominal
>    struct/ADT to a kind-only `TY_STRUCT`/`TY_ADT`. The emitter's
>    `type_c_name` then rendered the env field as `int64_t`, so the let-binding
>    init truncated the struct value and the inner call passed an `int64_t`
>    where the callee expected the nominal struct (a hard C compile error).
>    Fix: when `PAP_SLOT_FULL(i)` yields a `TY_STRUCT`/`TY_ADT` full type, the
>    capture binding now carries that full type, so the env field, the
>    let-binding, and the inner call all agree.
> 2. **Inner-`fn` capture of a by-pointer struct *parameter***
>    (`src/compiler/emit_expr.c`, `EX_CLOSURE` env-fill). A struct parameter
>    of the enclosing function crosses the pass-by-pointer ABI (`const T *`),
>    but the closure env field is declared by value (`T`). The capture-store
>    emitted `env->field = cn` -- assigning a pointer to a value field. Fix:
>    when the captured binding is one of the enclosing function's
>    `pbp_param_ptrs`, the store now derefs (`env->field = *cn`) so the closure
>    owns a copy (the caller's pointer would dangle after the frame returns).
>    This is the codegen behind the original `mw-cors` "destructure opts into
>    primitives so the inner closure env stays scalar-only" workaround.
>
> Coverage: `tests/fixtures/currying-partial-struct-capture` (small copy
> struct + struct/primitive mixed capture + struct-with-cstr, via `__pap`) and
> `tests/fixtures/closure-capture-byptr-struct-param` (inner-`fn` capture of a
> by-pointer struct param). The **stdlib `mw-cors` API rewrite** (B5) is left
> as its own follow-through: it additionally depends on compose-middleware's
> value-vs-name callability limitation (httpd.tur:3143), which is orthogonal
> to these codegen fixes.

### Phase B -- per-module follow-throughs (one PR each, gated on Phase A)

Each of these is a small, self-contained PR once its prerequisite lands.

- **B1. `arrow.tur` -- SPUN OFF.** The Arrow typeclass reintroduction is no
  longer tracked here. `__arrow_call1/2` are already gone -- the combinators
  now normalize their function arguments to fat closures via `^fat` params and
  dispatch through `TUR_APPLY1` (see the closure-dispatch note at the top of
  `stdlib/arrow.tur`). The disabled `defclass` scaffolding was removed in
  [stdlib-arrow-scaleback-plan.md](stdlib-arrow-scaleback-plan.md), and
  bringing the `Arrow`/`ArrowChoice`/... hierarchy *back* now lives in its own
  dedicated plan,
  [stdlib-arrow-typeclass-reintroduction-plan.md](stdlib-arrow-typeclass-reintroduction-plan.md)
  (gated on A1/A2/A3, all landed). Treat that plan as the source of truth.
- **B2. `effects.tur` -- LANDED.** The "handlers cannot capture" caveat is
  gone; `stdlib/effects.tur:12` now documents that handler bodies CAN capture
  (value, struct, loop-local, nested-handler), with the residual fat-closure
  env-leak caveat called out. Capturing-handler coverage lives in
  `tests/fixtures/effect-handler-capture-{loop,struct,nested}` (added under A4).
- **B3. `rc.tur` -- LANDED.** `Functor [rc]` is a real `definstance` that
  reaches the wrapped value through `tur_rc_ptr` and dispatches the mapping
  function via the fat-closure protocol (`fn.fn(fn.env, value)`) --
  `stdlib/rc.tur:58`. The earlier "can't reach the value through
  `RcControlBlock`" inline-C stub is retained only as the `__functor_rc_fmap`
  helper comment. `Foldable [rc]` and `Clone [rc]` round it out.
- **B4. `equal.tur` -- LANDED.** `equal-cong` is implemented and ungated
  (`stdlib/equal.tur:29`): `(defn equal-cong [^f eq : (Equal a b)] : (Equal (f a) (f b)) (match eq (Refl) (Refl)))`.
- **B5. `httpd.tur` -- LANDED.** Added `mw-cors-opts`, a struct-valued CORS
  factory: `(mw-cors-opts opts next)` returns a wrapped handler and the curried
  `(mw-cors-opts opts)` is a one-arg middleware usable directly in
  `compose-middleware`. The curried `__pap` env captures the `CorsOpts` value
  (the A5 fix). `compose-middleware` already accepts the partially-applied form
  -- it expands `(compose-middleware base (mw-cors-opts opts))` to
  `((mw-cors-opts opts) base)`, and calling a `__pap` expression in head
  position works -- so the previously-feared "value-vs-name callability"
  barrier did not materialize for this shape. The stale workaround comment on
  `mw-cors-with` was updated to point at `mw-cors-opts`. Coverage:
  `tests/fixtures/httpd-mw-cors-opts` (custom origin/methods threaded through
  the curried wrapper, via `compose-middleware`).
- **B6. `map.tur` -- STILL GATED.** The key-checking accessors remain macros
  (`stdlib/map.tur:392` documents why: a generic value of any type V must ride
  the map on the single int64 carrier word, which a typed `defn` would
  ABI-specialize away). Exposing them as real functions still needs the type
  checker to carry the key-type constraint through a normal `defn` (a separate
  typeclass-constraint-on-defn task; resolves open question #3 only once that
  lands).

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
2. ~~Does fixing A2 (closure-returning instance methods) also fix A5
   (struct-in-`__pap` env), or are they distinct codepaths in codegen?~~
   **Resolved (2026-06-04): distinct codepaths.** A2 touched dict-field /
   instance-method return-type propagation; A5 was two unrelated defects --
   `__pap` capture-binding type erasure (`elab_call.c`) and inner-`fn`
   by-pointer-struct-param capture stores (`emit_expr.c`). Neither was fixed
   by A2; both were fixed independently under A5.
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
