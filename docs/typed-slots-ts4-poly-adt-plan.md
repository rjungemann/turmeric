# Plan: Polymorphic ADT monomorphisation (TS4 follow-up)

> **Status:** Deferred — follow-up to TS4's concrete-payload slice
> **Parent:** [`route-b-typed-slots-plan.md`](../route-b-typed-slots-plan.md)
> **Type:** Compiler (codegen) + elab

---

## Why this is a separate plan

TS4's stated goal in the route-b plan is:

> `(defdata Maybe [a] (Just a) Nothing)` with concrete `a` emits a payload
> field of the right C type.

There are two readings of "with concrete `a`":

1. **Concrete payload at the declaration site.** The ADT's *declared*
   field type is a primitive keyword (e.g. `(defdata MaybeF (JustF :float))`).
2. **Concrete instantiation at the use site.** The ADT declares a type
   parameter (`[a]`), and is *used* at a concrete `a` (e.g.
   `(Just 1.5)` where `a = :float`).

The concrete-payload reading (1) already works end-to-end:
`emit_module.c:658-740` emits `struct { double _0; }` for a `:float`
field, the constructor takes `double` directly, and `match`
destructures into a typed local.  That slice is locked down by
`tests/fixtures/typed-slots/adt-float-payload/` and marked complete in
TS4.

The polymorphic-instantiation reading (2) is the substantial work,
because it requires the same kind of per-use-site monomorphisation
machinery that GS5 built for parameterised structs (`Cons__float`,
`Pair__int__float`, etc.).  For ADTs, the equivalent surface is:

- `tur_adt_Maybe__float { int tag; union { struct { double _0; } Just; ... } as; }`
- `ctor_Just__float(double _0) -> tur_adt_Maybe__float *`
- per-instance `match` that switches on the typed payload type rather
  than the int64 carrier.

Until this plan lands, polymorphic ADTs at concrete primitive types
remain on the int64 carrier; the TS3.3 ascribe-reinterpret pattern
(`(Just (:: f :int))` going in, `(:: x :float)` coming out) is the
supported boundary, exercised by
`tests/fixtures/typed-slots/ascribe-reinterpret/`.

---

## Goals and non-goals

### Goals

1. Per-use-site monomorphisation of polymorphic ADTs, mirroring the
   GS5 struct-app instantiation work.  `(Just 1.5)` where the
   surrounding context fixes `a = :float` emits a typed
   `ctor_Just__float` call into a `tur_adt_Maybe__float` heap cell.
2. Typed `match` destructure for monomorphised ADTs — the bound
   variable's local picks up the concrete C type (`double x_606`),
   not an `int64_t` carrier read.
3. Cross-boundary reinterpret insertion: when a typed-instance ADT
   flows into a polymorphic position, the compiler inserts the
   appropriate `EX_REINTERPRET` (TS2) at the boundary so the int64
   carrier ABI for the polymorphic site is preserved without user
   ascription.

### Non-goals

- Removing the int64 carrier ABI for polymorphic ADTs entirely.
  Genuinely polymorphic positions (HKT, existentials, generic
  containers used at unknown element types) still need it as a
  fallback.
- Existential / GADT specialisation.  Those carry their own
  representation contracts (`tur_existential_t`, skolem environments)
  that interact with the discussion below in non-obvious ways.  Out
  of scope until the plain-ADT case ships.
- Re-running TS3.3's container-boundary reinterpret work for ADTs.
  TS3.3 already established the codegen for `EX_REINTERPRET`; the
  work here is wiring it up at ADT-instance boundaries.

---

## Prerequisites

- TS3 fully landed (it is): the GS5 struct-app machinery (`type_app`
  registration, mangled names like `Pair__int__float`, per-instance
  field-layout codegen) is the model to mirror.
- TS2 reinterpret IR node and codegen landed (it is).
- An audit of how polymorphic-ADT use sites carry their concrete
  type-arg list to codegen.  Currently `EX_CALL` with `fn_binding`
  resolving to a ctor emits `ctor_<Name>(<args>)` with no per-instance
  suffix; the per-instance type information must be threaded through
  the call site to choose the right mangled ctor.

---

## Phase plan (sketch — refine when prerequisites are confirmed)

### Phase TS4P1 — ADT-app registration

Mirror `register_struct_app` in `types.c`: a global table that maps
each observed `(Maybe :float)` instantiation to a mangled name
(`tur_adt_Maybe__float`), with the canonical clone/eq machinery the
struct-app side already uses.  Emit each unique instance once in the
pre-decl section of the C output.

### Phase TS4P2 — Per-instance constructor codegen

For each registered ADT-app, emit a typed constructor function:

```c
static int64_t ctor_Just__float(double _0) {
    tur_adt_Maybe__float *__r = ...;
    __r->tag = 0;
    __r->as.Just._0 = _0;
    return (int64_t)(intptr_t)__r;
}
```

At the call site (`emit_expr.c:1403-1429`), choose between the legacy
`ctor_<Name>` and the per-instance `ctor_<Name>__<tyargs>` based on
the elaborated result type's `TY_APP` head.

### Phase TS4P3 — Per-instance match destructure

`emit_expr.c:3174` already unwraps `TY_APP` to find the base
`TY_ADT`.  Extend that path to thread the concrete tyarg list into
the destructure-binding type so the per-arm locals pick up the typed
C name (`double x_606`) instead of the carrier read.

### Phase TS4P4 — Boundary reinterprets

At polymorphic↔concrete ADT boundaries (e.g. a `Maybe__float` flowing
into a `Maybe<a>`-typed param), insert `EX_REINTERPRET` for the
payload-carrying field.  Heap-pointer-shaped ADTs don't need the
reinterpret for the pointer itself (`int64_t` already carries
`intptr_t` losslessly); the reinterprets apply to scalar payloads
when they're read out through a polymorphic match arm.

### Phase TS4P5 — Fixtures

- `adt-float-payload-poly.tur` — `(defdata Maybe [a] (Just a) Nothing)`
  used at `:float` round-trips without user bit-cast or ascribe.
- `adt-poly-boundary.tur` — `Maybe__float` through a
  `[A] [m :(Maybe A)] :(Maybe A)` identity fn and back; payload read
  yields a `double` at both ends.

---

## Risks / open questions

- **GADT interaction.** `is_gadt` ctors carry their own
  `result_type_form` and skolem environment; the monomorphisation
  story for them is non-obvious.  Defer GADTs until the
  `defdata`-only case is solid.
- **Drop glue divergence.** `needs_drop_glue` is currently a single
  flag on `AdtDef`.  If one instantiation has an rc-payload type and
  another does not, the drop glue needs to be per-instance.  Audit
  before committing to the layout.
- ~~**`tag` discriminant size.**~~ *Resolved.* Monomorphisation only
  substitutes payload C types inside the union; ctor count (and the
  tag range) is per-ADT and identical across instances, so `int tag`
  stays. Corollary for TS4P4: every instance leads with `int tag;`
  followed by the union, so a `tur_adt_Maybe__float *` and a
  `tur_adt_Maybe *` agree on the tag slot's offset — tag-reads are
  ABI-stable across instances, and the boundary reinterprets are
  payload-only.

---

## Acceptance criteria

- All five fixtures above pass.
- Full `tests/run.sh` regression unchanged at baseline (884 passed
  / 110 failed per KB-006 at the time of writing — re-baseline
  against the current main).
- No new union/bit-cast in user code for the round-trip of a `:float`
  payload through a polymorphic ADT.
- Signal-spice migration (TS6) does not need this plan to land; it
  unblocks the eventual retirement of the TS3.3 ascribe pattern at
  polymorphic-ADT boundaries.
