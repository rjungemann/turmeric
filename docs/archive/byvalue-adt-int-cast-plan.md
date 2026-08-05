# By-value ADT `::` carrier cast (GAP 3) -- Plan

> **Status:** DONE 2026-07-13. Part A (diagnostic) and Part B (the boxing
> bridge) both landed. Key finding during Part B: the sound bridge **already
> existed** as `any` + `cast`; the fix was to make `(:: v :any)` box correctly
> (it was miscompiling) so there is a clean explicit spelling, point the
> `TUR-E0295` diagnostic at it, and NOT mint new `box`/`unbox` verbs (they
> collide with `stdlib/safe.tur`'s existing `box`/`unbox` int-cell functions).
> **Re-verified 2026-07-19:** state holds at HEAD. `TUR_E0295_BYVALUE_CARRIER_CAST`
> is registered (`diag.h`/`diag.c`) with an `explain` entry; the
> `ascribe_type_is_byvalue_aggregate` / `ascribe_type_is_word_carrier` helpers
> and the `elab_ascribe` rejection both live at `src/compiler/elab_types.c:2503+`,
> and `(:: v :any)` routes through the coerce path there. All three fixtures
> present: `tests/fixtures/errors/byvalue-adt-cast-to-int`,
> `.../int-cast-to-byvalue-adt`, and `tests/fixtures/box-unbox-byvalue-aggregate`.
> Only Phases 3 (cosmetic `logic.tur` follow-up) and 4 (deferred B2 auto-box)
> remain, both explicitly optional/out-of-initial-scope -- nothing tracked as
> in-scope work is open. Ready to archive.
> **Last Updated:** 2026-07-19
> **Type:** compiler / type-erasure soundness + ergonomics
> **Scope:** Make `(:: v :int)` / `(:: v :ptr<void>)` (and the reverse) *sound*
> when `v` is a non-recursive by-value ADT/struct: today it silently miscompiles.
> Land a diagnostic first (kills the miscompile on both harnesses), then an
> explicit, safe heap-boxing bridge so a by-value aggregate can genuinely ride an
> erased carrier when a caller needs it.
> **Headline finding:** the machinery already exists (`emit_agg_box` /
> `emit_agg_unbox`, used at every poly-carrier / `any` / lens crossing); `::` is
> the one erased-carrier boundary that neither uses it nor rejects the cast.
> Tracked as GAP 3 in `docs/reported/logic-port-language-gaps.md`.

---

## Motivation

A non-recursive flat-product ADT/struct is represented **by value** (a C
aggregate `tur_adt_P`), not as an int64 handle -- see `adt_is_byvalue_product`
(`src/compiler/types.c`) and `type_uses_carrier_abi` (`src/compiler/emit_core.c:341`).
A *recursive* ADT (a field mentions `Self`) rides the int64 carrier, so it casts
to/from `:int` cleanly; a by-value one has no handle to cast to. `::` does not
account for this, so both directions are broken today, **differently on each
harness**:

```turmeric
(defdata P :copy (P :int :int))
(:: (P 3 4) :int)     ; compiled: cc error "aggregate value used where an integer
                      ;           was expected"
                      ; --interpret: prints a raw handle int (garbage, no error)
```

```turmeric
(defn from-int [h : int] : P (:: h :P))
(from-int 0)          ; compiled: SIGSEGV (reads an int as a 2-word struct)
                      ; --interpret: runtime "match: no arm matched"
```

Neither path is right, and they disagree -- a type-erasure hole that leaks a raw
`cc` error one way and silently miscompiles the other. It surfaced in the
`stdlib/logic.tur` port: a natural `UState { subst next }` product could not ride
the goal-closure `:int` carrier, forcing the fresh-counter to be folded into the
recursive `Subst` base instead (correct, but shaped around this gap). More
generally, any code that threads a small by-value aggregate through an erased
handle (a callback carrier, an opaque payload, a hand-rolled tagged carrier) hits
this.

Because the elaborator is shared by both harnesses, a fix in `elab_ascribe`
corrects the compiled path **and** the interpreter at once.

---

## Root cause

`elab_ascribe` (`src/compiler/elab_types.c:2476`) handles three cast shapes and
misses this one:

1. **Same-size scalar reinterpret (TS3.3, ~line 2581).** Gated on
   `type_size_bytes(src) > 0 && type_size_bytes(dst) > 0`. A by-value aggregate
   and `TY_FN`/`TY_APP` all report size 0, so this block is **skipped**.
2. **Fat-handle ascription (~line 2602).** Marks a `:int`/`:ptr<void>`/opaque
   carrier ascribed *to* a `fn` type as `boxed`. Only relevant for `fn` targets.
3. **Fallthrough.** Everything else becomes a bare `EX_ASCRIBE` whose type is
   overridden and *erased at codegen*. For a by-value aggregate ↔ `:int` that
   means emit tries to (a) assign an aggregate into an `int64_t` slot -> the
   `cc` error, or (b) treat an `int64_t` as the aggregate struct -> the segfault.

The value *should* cross an int64 carrier the way it already does at a poly
parameter: `emit_agg_box` (`src/compiler/emit_expr.c:503`) mallocs + copies and
yields the `int64` pointer; `emit_agg_unbox` (`:515`) derefs it back. `::` is
simply not wired to that bridge, and does not reject the cast either.

---

## Design

Two parts. Part A is a soundness fix (small, low risk, ship first). Part B is the
capability (opt-in, explicit).

### Part A -- diagnose the unsound direct `::` (soundness, priority)

In `elab_ascribe`, once both `inner->type` and `ascribed` are known, detect the
ill-defined shape and emit a real Turmeric diagnostic instead of falling through:

- **`(:: agg :int)` / `(:: agg :ptr<void>)`** where `agg` is a by-value
  aggregate (`type_uses_carrier_abi(inner->type) == false` **and** it is a
  by-value ADT/struct, i.e. `adt_is_byvalue_product` / the struct analogue), and
- the reverse **`(:: carrier :Agg)`** where the source is a one-word carrier and
  the target is a by-value aggregate.

Message (new code, e.g. `TUR-E02xx`): explain that a by-value aggregate has no
one-word carrier to reinterpret, and point at the Part B boxing bridge
(`(box ...)` / `(unbox ...)`), mirroring how `docs/reported/...` references work.
Do **not** silently succeed. This alone closes the miscompile on both harnesses.

Per the "Experimental Compiler Features" STRICT RULE, diagnostic strictness is
explicitly out of scope for `--enable` gating, so Part A ships gateless.

### Part B -- an explicit, safe heap-boxing bridge (capability)

Provide a first-class way to cross the boundary intentionally, reusing the
existing bridge so there is no new codegen concept:

- `(box expr)` : `T` (by-value aggregate) -> `:ptr<void>` handle. Lowers to
  `emit_agg_box` (malloc + copy, yield the `int64` pointer).
- `(unbox expr T)` : `:ptr<void>`/`:int` handle + target aggregate type `T` ->
  `T`. Lowers to `emit_agg_unbox` (deref).

Naming/surface options to decide during Phase 2:

- **B1 (recommended): explicit `box`/`unbox` forms.** Keeps `::` a pure
  reinterpret (its documented meaning), makes the heap allocation visible and
  greppable, and is symmetric with the existing `reinterpret`/`unsafe` verbs. A
  `#box{...}` reader form is an alternative spelling if a keyword collides.
- **B2 (deferred): teach `::` to auto-insert box/unbox** when it sees a by-value
  aggregate ↔ erased carrier. Most ergonomic, but it makes `::` silently
  allocate, which is surprising for a "reinterpret" operator and hides the
  ownership question below. Revisit only if B1 proves too noisy in practice.

Recommendation: **A + B1.** `::` stays honest; boxing is opt-in and explicit.

**As landed (supersedes B1/B2 above).** Prototyping revealed the sound bridge
*already ships* -- the `any` type is a one-word tagged heap handle, `elab_coerce_to_any`
(EX_UNION_INJECT) heap-boxes a by-value aggregate into it, and `(cast h T)`
(EX_ANY_CAST) reads it back by value; both already run under the compiled path
and `--interpret`. So no new box/unbox machinery was needed. Two things were
done instead:

- **Fixed `(:: v :any)` to coerce.** It previously just *relabelled* the value's
  static type to `any` without inserting the box, which miscompiled a by-value
  aggregate (a cc error compiled, a divergent result under `--interpret`).
  `elab_ascribe` now routes `(:: v :any)` through `elab_coerce_to_any`, giving a
  clean **explicit box spelling**. `(cast h T)` is the unbox.
- **Did NOT add `box`/`unbox` verbs.** A first cut added them as special forms,
  but `stdlib/safe.tur` already defines `box : int -> ptr` / `unbox : ptr -> int`
  (an int-cell abstraction) and global special-form dispatch shadowed them,
  breaking the `safe-box` fixture. The `any` + `cast` surface is the bridge; the
  `TUR-E0295` diagnostic points straight at it.

This lands closer to B2's ergonomics (an existing operator does the boxing) than
B1, but without the "silent surprising allocation" concern: `(:: v :any)` is an
*explicit* coercion into a type whose whole purpose is erased dynamic carriage,
and the implicit-coercion path is the same one every `any`-typed parameter
already uses. Ownership is inherited from `any` (unchanged), so the lifetime
question below needed no new decision.

### Ownership / lifetime (must be pinned in Phase 2)

`emit_agg_box` mallocs. The existing poly-carrier / `any` / lens boxes are
process-lifetime (never freed) -- acceptable there because they are transient and
the compiler's own path is what the leak gate checks (`bash tests/run.sh` runs
`tur build`/`emit-c` under LSan; the *emitted user program* is built `-O2` with
no sanitizer, per `tests/run.sh`). A user-facing `box` would inherit that
"leaks by default" convention, which is a poor contract for a public verb.
Decide one of:

1. **Owned handle + explicit free** -- `box` returns a handle the caller frees
   (a `(box-free h)` or via `drop`); documents the allocation honestly.
2. **`:copy`-only + drop-glue integration** -- restrict `box` to
   drop-glue-free `:copy` aggregates and hang the free off the handle's scope
   (harder; needs the handle to carry a type tag).
3. **Process-lifetime, documented** -- match the existing carrier boxes; only
   acceptable for small/bounded use, and must be called out so it never lands in
   a hot loop.

Lead with option 1 for a public verb; keep the internal `emit_agg_box` callers
unchanged.

### Interpreter (turi) parity

The tree-walking interpreter has no by-value/handle split -- an ADT is a
`TuriValue`. Today the naive `::` "works but returns garbage" one way and throws
the other. Both `box`/`unbox` must be given interpreter semantics (a native or an
`eval.c` arm) so a round-trip is identity there, and the Part A diagnostic --
being in the shared elaborator -- already fires under `--interpret`. A fixture
must run green under both harnesses (`tests/run.sh` + `tests/run-turi.sh`) and the
parity checks (`tools/check_turi_parity.py`, `check_turi_native_parity.py`) must
stay 0-gap.

---

## Phases

### Phase 1 -- diagnostic (Part A) -- DONE 2026-07-13

- `elab_ascribe` (`src/compiler/elab_types.c`) now rejects `::` between a
  by-value aggregate and a one-word carrier in either direction, via the new
  `ascribe_type_is_byvalue_aggregate` + `ascribe_type_is_word_carrier` helpers,
  raising `TUR-E0295` (new code, registered in `diag.h`/`diag.c` with an
  `explain` entry). Scoped to the **non-parametric** `TY_ADT` by-value product
  (the reported shape): `adt_is_byvalue_product` already requires
  `n_type_params == 0`, and a parametric `(Option int)`-style monomorph is
  deliberately left alone (HKT/typeclass code legitimately erases those to the
  carrier). Opaque, `:heap`, and recursive ADTs are excluded and still cast
  soundly.
- Negative fixtures: `tests/fixtures/errors/byvalue-adt-cast-to-int` and
  `.../int-cast-to-byvalue-adt`; both green under `run.sh` and `run-turi.sh`
  (the diagnostic, being in the shared elaborator, fires identically under
  `--interpret`).
- Full suite: 2127 passed, 0 failed -- no legitimate cast was caught; parity
  checks 0-gap.

### Phase 2 -- the boxing bridge (Part B) -- DONE 2026-07-13

Landed as the `any` + `cast` bridge rather than new verbs (see "As landed"
above):

- `elab_ascribe` (`src/compiler/elab_types.c`) routes `(:: v :any)` through
  `elab_coerce_to_any` so it heap-boxes instead of relabelling -- the explicit
  box spelling. `(cast h T)` is the unbox. No new EX kinds, no interpreter
  natives, no parity-carve changes (EX_UNION_INJECT / EX_ANY_CAST already run on
  both harnesses).
- `TUR-E0295`'s message and `explain` text point at `(:: v :any)` + `(cast h T)`.
- Round-trip fixture `tests/fixtures/box-unbox-byvalue-aggregate` covers a
  2-field ADT (explicit `:: :any`) and a wide 3-field struct (implicit coercion),
  green on both harnesses.
- Ownership is inherited from `any` (process-lifetime, unchanged) -- no new
  contract, so the lifetime discussion below did not need a decision. The
  compiler leak gate stays green.

### Phase 3 -- opportunistic `logic.tur` follow-up (optional, cosmetic)

- Only if it reads better: replace the counter-in-`SNil` encoding with a natural
  `UState { subst next }` boxed through the goal carrier. The current encoding is
  correct; this is a readability call, not a fix. Regenerate no snapshots unless
  codegen moves.

### Phase 4 -- transparent `::` auto-box (B2, deferred)

- Revisit only if B1 is too verbose in practice. Would auto-insert box/unbox at
  `::` boundaries; requires resolving the ownership contract for an *implicit*
  allocation first. Not in the initial scope.

---

## Validation / definition of done

- No `::` between a by-value aggregate and a one-word carrier reaches codegen:
  it is either a Part A diagnostic or an explicit `box`/`unbox`.
- The two GAP 3 repros above no longer miscompile: forward and reverse both
  produce the *same* clear diagnostic on compiled and `--interpret`.
- `(unbox (box v) T)` round-trips by value on both harnesses.
- `bash tests/run.sh` and `bash tests/run-turi.sh` green; `check_turi_parity.py`
  and `check_turi_native_parity.py` 0-gap.
- Ownership contract documented on the `box`/`unbox` docstrings; no new leak in
  the compiler/codegen path (the leak gate stays green).

---

## Risks

- **Blast radius of the diagnostic.** Some existing `::` might rely on the
  accidental fallthrough. Unlikely (it miscompiled), but audit stdlib + fixtures
  for `(:: <aggregate> :int/:ptr<void>)` before landing Phase 1.
- **Ownership leak** if Part B ships as process-lifetime; hence the explicit
  contract decision up front.
- **Interpreter divergence.** `box`/`unbox` must be shimmed so a round-trip is
  identity; otherwise the harnesses disagree again.
- **Double-boxing.** Do not let a Part B `box` compose with the existing
  auto-boxing at poly-carrier sinks (which already box a by-value aggregate). The
  bridge is the same helper, so a boxed handle passed to a poly param must not be
  re-boxed -- reuse the `boxed`/carrier flags the poly path already checks.

---

## Alternatives considered

- **Diagnostic only (Part A, no Part B).** Safe and cheap, but leaves no way to
  legitimately carry a by-value aggregate through an erased handle -- callers
  must restructure (as `logic.tur` did). Acceptable as a first landing; Part B is
  the completion.
- **Make all non-recursive ADTs int-boxed always.** Removes the gap by fiat but
  regresses the by-value representation that exists for performance/layout
  reasons across the whole codebase. Rejected.
- **Transparent `::` boxing (B2) as the primary fix.** Most ergonomic but makes a
  "reinterpret" operator silently allocate and hides ownership. Deferred behind
  B1.

---

## See Also

- `docs/reported/logic-port-language-gaps.md` -- GAP 3 (this plan is its fix).
- `src/compiler/elab_types.c:2476` (`elab_ascribe`) -- the cast site.
- `src/compiler/emit_expr.c:503` (`emit_agg_box` / `emit_agg_unbox`) -- the
  existing heap-box bridge to reuse.
- `src/compiler/types.c` (`adt_is_byvalue_product`),
  `src/compiler/emit_core.c:341` (`type_uses_carrier_abi`) -- the by-value vs
  carrier representation predicates.
