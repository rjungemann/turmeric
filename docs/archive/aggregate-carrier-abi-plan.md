# Plan: Aggregate Carrier ABI Unification

> **Status:** Draft Plan
> **Last Updated:** 2026-05-27
> **Type:** Compiler / Codegen
> **Tracks:** KB-004, KB-010, KB-012, KB-015 (see `docs/known-bugs.md`)

---

## Overview

Turmeric's codegen carries values through two parallel ABIs:

1. **Carrier ABI** -- aggregates flow as `int64_t` slots that hold either an
   inline 8-byte payload or a heap pointer cast to `intptr_t`.  Almost all
   stdlib helpers (`ok`, `err`, `vec_new`, `make_struct` thunks, etc.)
   return values via this ABI.
2. **Concrete C ABI** -- monomorphised specialisations declare the C struct
   type directly (`Result__int__int`, `Vec__int`, `Cons__double`) and pass
   it by value.

The compiler picks an ABI per call/binding site by inspecting the
elaborated `Expr->type`, but four distinct sites still get it wrong.  The
result is C-level type errors (KB-004, KB-010), wrong-width memory reads
(KB-014, now fixed), and bit-reinterpret bugs producing denormal garbage
(KB-015), as well as straight segfaults when the convention mismatch
sits inside a typeclass instance body (KB-012).

This plan unifies the conversion logic into a single
`emit_carrier_bridge` helper that every emit site routes through, and
fixes the four known bug clusters in one pass.

---

## Bug clusters covered

### Cluster A -- KB-004: `::` coercion does not insert a pointer cast

`(:: (result-map f r) (Result int int))` reinterprets the expression's
type to a specialised struct, but the value flowing in is still an
`int64_t` heap-pointer carrier.  Downstream `ok-val` specialisation
takes the struct by value and rejects the `int64_t` at the C level.

### Cluster B -- KB-010: `vec-new` returns carrier where struct expected

`(let [v :(Vec int) (vec-new)])` emits
`Vec__int v_532 = vec_new();` -- `vec_new()` returns `int64_t`.
Same shape as KB-004 but driven by `let`-binding type annotation rather
than `::`.

### Cluster C -- KB-012: Typeclass instance body crashes on parametric struct

`(.eq? t1 t2)` for a `Tuple2[int int]` segfaults because the
specialised `eq?` body receives parameters under one convention
(by-value struct) and dereferences them as if they were the other
(pointer carrier).  Field-offset arithmetic on a non-pointer is a SEGV.

### Cluster D -- KB-015: Repeated specialised accessor call wraps in reinterpret

The second call to `(thead ys)` in the same scope wraps the (already
correct) `double`-returning specialisation in a `int64_t <-> double`
union reinterpret, truncating the result.  Specialisation-cache
state in `elab_call.c` reuses a carrier-typed result-type slot across
calls.

---

## Root cause synthesis

All four bugs share one shape: **two pieces of state disagree about
what ABI is in play at the boundary between a generic call site and a
concrete consumer**.

- KB-004 / KB-010: the *value-side* still produces the carrier; the
  *type-side* says concrete struct.  No cast bridges the two.
- KB-012: the *parameter-side* of the specialised instance body uses
  the carrier; the *body-side* dereferences as concrete struct.
- KB-015: the *first* call resolves to concrete; the *second* call
  reads stale carrier state from the specialisation cache.

Today the emit code makes these decisions ad-hoc at each site (in
`elab_call.c`, `emit_stmt.c`, `emit_expr.c`).  There is no single
chokepoint where "carrier-in, concrete-out" or vice versa is named
and the bridge cast is inserted.

---

## Plan

### Phase 1 -- Audit and classify call sites

1. Grep `src/compiler/emit_*.c` for every site that emits a value of
   aggregate type.  Classify by:
   - Source ABI (what the producer returns: carrier or concrete)
   - Sink ABI (what the consumer expects)
2. Enumerate them in `docs/aggregate-carrier-abi-audit.md` (a checklist
   file produced by Phase 1 -- not landed yet).

Initial candidate sites:
- `elab_call.c` around the `call_wrap_reinterpret` site (KB-015 lives here)
- `elab_ascribe.c` -- `EX_REINTERPRET` insertion for `::`
- `emit_stmt.c` -- `let`-binding C-type selection (KB-010, KB-014 lived here)
- `emit_expr.c` -- `make-struct` result type vs. surrounding context
- The instance-specialisation entry in `emit_fns.c` (KB-012)

### Phase 2 -- Introduce `emit_carrier_bridge`

Add a single helper in `src/compiler/emit_core.c`:

```c
// Emit value `e` so its surface form matches `sink_kind` at the C level.
// Handles all four combinations of (carrier, concrete) X (carrier, concrete).
// No-op when the source and sink agree.
const char *emit_carrier_bridge(Emit *em,
                                const Expr *e,
                                CarrierKind src_kind,
                                CarrierKind sink_kind,
                                const Type *concrete_ty);
```

`CarrierKind` is a new two-state enum (`CK_CARRIER`, `CK_CONCRETE`).
The bridge function knows how to emit the four cast forms:

- `CK_CARRIER -> CK_CONCRETE`:
  `(*(ConcreteType *)(intptr_t)src_expr)` (pointer carriers) or
  `((union { int64_t s; ConcreteType d; }){.s = src_expr}).d`
  (inline 8-byte payloads).  The choice depends on `concrete_ty`'s
  layout, available via `type_layout(concrete_ty)`.
- `CK_CONCRETE -> CK_CARRIER`:
  `(int64_t)(intptr_t)&temp` after writing `temp = src_expr` to a
  fresh local, or a direct `(int64_t)(intptr_t)src_expr` when the
  source is already an lvalue with stable address.
- Same-to-same: emit `src_expr` verbatim.

### Phase 3 -- Route every aggregate site through the bridge

For each site enumerated in Phase 1, replace the ad-hoc cast logic
with a call to `emit_carrier_bridge`.  This is mechanical once the
helper exists.

This phase fixes KB-004 and KB-010 directly: the let-binding /
ascription paths now detect the carrier-to-concrete crossing and emit
the dereference, instead of leaving an `int64_t = struct` mismatch.

### Phase 4 -- Pick one convention for parametric typeclass instances

KB-012 needs a *choice* (not just a bridge): every parametric
`definstance` body must agree with its specialisations on whether
arguments arrive carrier-typed or concrete-typed.

Recommendation: **concrete by-value**, because:
- It matches how non-instance specialisations already work.
- It removes a layer of dereference inside hot inner loops
  (`eq?`, field-access).
- The bridge in Phase 2 handles call sites that still ship the carrier.

Concretely:
1. Change `instance_specialise` in `src/compiler/elab_typeclass.c` so
   the generated body declares its parameters using the concrete C
   struct type for the dispatching argument.
2. Audit `Eq [Pair]` (`stdlib/pair.tur:113`), `Eq [Option]`
   (`stdlib/option.tur:173`), and `Eq [Tuple2]` (`stdlib/tuple.tur`)
   for the new convention.  Drop the inline-C "cast int64_t to
   struct*" carriers; they're now redundant.
3. Each call site that today passes the carrier must now go through
   `emit_carrier_bridge`.  This is the Phase 3 mechanism kicking in.

### Phase 5 -- Flush the specialisation result-type cache

KB-015 is a cache freshness bug, not a convention bug.  The fix is
narrow:

1. In `src/compiler/elab_call.c` around line 1687 (the
   `call_wrap_reinterpret` site), confirm whether the `result_type`
   passed in is derived from the *binding* type (carrier-shaped) or
   the *instantiation* type (concrete-shaped).
2. If the binding type, rederive from the call's resolved
   instantiation each time -- do not cache across call sites.
3. Add a focused regression fixture
   `tests/fixtures/typed-slots/cons-double-twice/` that calls
   `thead` on two bindings of the same instantiation and asserts
   both stdout values.

This phase is independent of Phases 2-4 and can land first.

### Phase 6 -- Regression coverage

Add fixtures that exercise each crossing combination:

| Fixture | Crossing | KB |
|---|---|---|
| `coerce-carrier-to-struct/` | `result-map :int -> (Result int int)` accessor | KB-004 |
| `let-vec-new/` | `(let [v :(Vec int) (vec-new)] ...)` | KB-010 |
| `tuple2-eq-method/` | `(.eq? t1 t2)` on parametric `Tuple2` | KB-012 |
| `cons-double-twice/` | `(thead xs)` twice on `(Cons double)` | KB-015 |

Each fixture should compile cleanly (no C-level type warnings) and
emit the expected runtime output.

---

## Out of scope

- Generalising the carrier/concrete enum to a kind system: this plan
  uses a binary tag.  A richer ABI descriptor (carrier-with-tag,
  inline-or-pointer, etc.) is a future refactor.
- Inline-C helpers that intentionally use the carrier ABI for opaque
  handles (`vec_new`, `hamt_*`, etc.) keep their current signatures.
  The bridge only fires when crossing into typed code.

---

## Risks

- **Behavioural change at every let-binding for aggregates.**  Phase 3
  rewrites the local-declaration type from `int64_t` to the concrete
  struct.  Existing inline-C that reads `int64_t` locals via a
  reinterpret cast will break.  Mitigation: audit inline-C blocks
  before/after Phase 3 (probably `stdlib/vec.tur`, `stdlib/hamt.tur`).
- **Specialisation explosion.**  Concrete by-value instance bodies
  generate one C function per instantiated type.  Today the carrier
  form lets a single body service many instantiations.  Mitigation:
  measure C-file size before/after on a representative project; if
  the blowup is bad, gate Phase 4 behind a flag.

---

## Verification

Each phase has a green/red signal:

- Phase 1: audit doc exists in `docs/aggregate-carrier-abi-audit.md`
  with at least the five known sites enumerated.
- Phase 2: `emit_carrier_bridge` compiles; a unit test in
  `tests/compiler/test_emit_carrier_bridge.c` covers all four
  conversion directions.
- Phase 3-4: KB-004, KB-010, KB-012 fixtures pass.
- Phase 5: KB-015 fixture passes.
- Phase 6: all four regression fixtures pass in both `run.sh` and
  `run-turi.sh`.

---

## Open questions

1. Does `make-struct` always produce a carrier today, or does it
   already emit concrete for fully-instantiated calls?  (KB-014's
   fix suggests the latter for compound literals; KB-010 suggests
   the former for handle-returning functions.  Phase 1's audit
   will pin this down.)
2. For the `CK_CARRIER -> CK_CONCRETE` direction, is the underlying
   payload always a heap pointer, or sometimes an inline 8-byte
   value?  The bridge needs to emit different cast forms for the
   two cases.  Phase 2's layout query will answer this.
