# `:sealed` opaque newtypes -- encapsulation the `::` cast cannot fabricate

**Experiment:** `sealed-opaque` (`--enable=sealed-opaque`, or `:experiments`
in `build.tur`).
**Status:** in-flight prototype. Introduced 0.32.2; `expires_at` 0.35.0.
**Motivating report:**
[docs/reported/frozen-region-aliasing-via-coercing-cast.md](../reported/frozen-region-aliasing-via-coercing-cast.md).

## The problem

`::` is a **coercing** cast -- turmeric's carrier-interop escape hatch -- not a
checked ascription. A `defopaque` therefore does not encapsulate its handle: any
caller can unwrap it to the representation type and re-wrap the result as a
fresh value of the opaque type. Both directions compile, in any module.

That is fine for interop, but it silently bounds every guarantee built on top of
a `defopaque` handle. The concrete case that motivated this is the ECS spice's
`frozen` region. Its soundness argument is "while `(& w)` is live, no mutating
handle to the frozen world can be acquired, because every mutator takes
`^unique ^mut w` and uniqueness forbids a second mutable handle." That argument
is correct, and `::` walks straight around it:

```turmeric
(let [__b (& w)]                    ; w is now immutably borrowed
  (let [w2 (:: (:: w :int) H)]      ; unwrap to the carrier, re-wrap as a NEW handle
    (h-bump! w2)))                  ; w2 is OWNED, not the borrowed w -- no TUR-E0200
```

Measured on 0.32.2: mutating the borrowed `w` directly is correctly rejected
with `TUR-E0200`; the aliased `w2` compiles clean, runs, and the mutation is
observable through the live borrow. A `defstruct` field (`(.ctrl w)` plus a
re-construction) is the same hole through a different door.

## The design

Add a `:sealed` attribute to `defopaque`:

```turmeric
(defopaque RGWorld :int :sealed)
```

Inside the module that declares it, `::` behaves exactly as today -- the module
needs to build and unwrap its own handles, and that is the whole point of an
opaque newtype having a representation at all.

Outside that module, `::` refuses **both** directions:

| use site | `(:: n RGWorld)` | `(:: w :int)` |
| --- | --- | --- |
| declaring module | allowed | allowed |
| any other module | **TUR-E0302** | **TUR-E0302** |

### Why both directions, not just fabrication

Blocking only fabrication (`int -> Sealed`) is enough to stop the aliasing hole
as written, because the alias cannot be rebuilt. It is not enough to be worth
calling encapsulation: once the raw carrier escapes, inline-C can do anything
with it, including handing it to a function that fabricates *inside* the
declaring module. Sealing the unwrap direction too keeps the representation an
implementation detail rather than a value the outside world is merely
discouraged from rebuilding.

This does mean `:sealed` is a stronger claim than "you cannot alias me." It is
"my representation is not part of my public API."

### What this does NOT claim

`:sealed` is a **compile-time** discipline over the `::` surface. It is not a
capability and not a runtime protection:

- inline-C in any module can still cast a `:ptr<void>` / `int64_t` to whatever
  it likes. That is the escape hatch inline-C has always been, and sealing does
  not touch it.
- A sealed handle passed to a function that takes the representation type still
  arrives as a plain integer; nothing stops that function.

So sealing raises the aliasing hole from "one `::` away, in ordinary code" to
"requires deliberate inline-C." That is the honest claim, and it is the claim
the ECS spice's TRUST BOUNDARY docstring should make once it adopts this.

## Semantics

1. **Attribute position.** `:sealed` is an optional attribute in the same slot
   as `:linear` / `:affine`, and **composes** with them:
   `(defopaque H :int :affine :sealed)`. Today that slot accepts exactly one
   attribute (`elab_structs.c`); this generalises it to a set, which is a
   prerequisite rather than an extra.

2. **Module identity.** A sealed opaque records the module that declared it.
   The check compares the elaboration site's current module against it by name.
   A `defopaque` at top level outside any `defmodule` belongs to the implicit
   top-level module; two different files both outside a `defmodule` are
   therefore *not* separated by this check. That is a known limitation, not a
   design goal -- it matches how the rest of the module system treats
   moduleless code, and single-file programs are exactly where sealing has the
   least to offer.

3. **Where the check fires.** In the `::` coercion path only, when one side is
   a sealed opaque and the other is its representation type. A cast between a
   sealed opaque and an *unrelated* type is already an error for its own
   reasons and is not this feature's business.

4. **Gating.** Every part of this is behind `g_opt_sealed_opaque`. With the
   experiment off: `:sealed` still parses (so a spice that adopts it does not
   fail to build for users who have not enabled it) but imposes nothing. This
   is deliberate -- a parse error would make adoption a breaking change for
   every downstream consumer, which is the opposite of what an opt-in prototype
   should cost.

## Diagnostic

`TUR-E0302` -- casting to or from the representation type of a sealed opaque
outside its declaring module. The message names the type, the declaring module,
and the direction, and points at this plan.

(Note for anyone extending the diagnostic space: `TUR-E0290`/`E0291` look free
in `diag.c` but are **taken** by the typed-field row literals -- they are
emitted as message text rather than registered enum cases, so grepping
`diag.c` alone under-reports what is in use. `0302`-`0309` were genuinely
unused.)

## Staging

- **S1** -- plan (this document) + `EXPERIMENTS[]` row + `g_opt_sealed_opaque`.
- **S2** -- multi-attribute `defopaque` parse; `:sealed` recorded on the
  `AdtDef` along with the declaring module.
- **S3** -- the `::` check + `TUR-E0312`; `experiment_warn_if_used` at the
  elaboration entry point so TUR-W0060/W0061 fire.
- **S4** -- fixtures: in-module cast allowed; out-of-module fabricate rejected;
  out-of-module unwrap rejected; gate-off compiles clean; `:sealed` composes
  with `:affine`.
- **S5** -- guide paragraph
  ([experimental-flags-guide](guides/experimental-flags-guide.md),
  [syntax-guide](guides/syntax-guide.md)).
- **S6** (separate repo) -- ECS spice adopts `:sealed` on `RGWorld` and its
  TRUST BOUNDARY docstring is rewritten to the narrower, true claim.

## Graduation criteria

Graduate (delete the row, behavior unconditional) when:

- the ECS spice has shipped on it and no legitimate in-module pattern has
  needed an escape, and
- the "moduleless top level" limitation in semantics (2) is either closed or
  explicitly accepted in the guide.

Shelve if it turns out that real spices routinely need to unwrap a sealed
handle across a module boundary -- that would mean the two-direction rule is
wrong and only fabrication should be sealed.

## Alternatives considered

- **Lint instead of error.** Rejected as the primary design: a warning does not
  restore the `frozen` guarantee, it only annotates its absence. (A warning
  *is* what the experiment-off path effectively provides, since nothing fires.)
- **Module-private constructor** (report's fix direction 1, other half).
  Strictly more general -- it would seal construction regardless of `::` -- but
  it needs a visibility system `defopaque` does not currently have. `:sealed`
  is the narrow version that fits the existing `:linear`/`:affine` slot.
- **Making `::` a checked ascription.** A much larger, breaking change to the
  language's escape hatch, and it would break legitimate carrier interop
  (`(:: (make-write-cap 0) (WriteCap Pos))`). Out of scope.
