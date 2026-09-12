# `spices/crdt`: conflict-free replicated data types

> **Status:** proposed (2026-09-11). **Track:** post-v1 -- nothing on the v1
> line depends on this; it is written down so the design survives.
> **Type:** spice (in `../turmeric-spices/`), plus one candidate stdlib
> addition (`map-merge-with`) that the plan deliberately declines to make.
> **Sequencing:** last of three.
> [type-confusion-detection-plan.md](type-confusion-detection-plan.md) ->
> [lattice-vocabulary-plan.md](lattice-vocabulary-plan.md) -> this. C1's
> `JoinSemilattice` comes from the lattice plan rather than being redeclared
> here, and both are gated on two open typeclass defects filed from this plan's
> own probes.

## 0. Summary

Turmeric has **no lattice vocabulary at all.** Greps for `Semigroup`,
`Monoid`, `semilattice`, `CRDT`, and `LWW` over `stdlib/`, `src/`,
`docs/guides/`, and all 45 spices return nothing. There is no `join`, no
`bottom`, and no class that expresses "these two values can be combined in
any order, any number of times, and you get the same answer."

That is a larger hole than it looks, because Turmeric already has the two
things a CRDT library normally has to build for itself:

- **Persistent state with snapshot semantics.** `stdlib/hamt.tur` is an
  immutable, structurally-sharing HAMT -- "all update operations return a
  new root; the old root remains valid". That is precisely the CvRDT state
  model, and `stdlib/set.tur` already exposes `set-union` / `set-intersect`
  / `set-diff` over it. A grow-only set is `set-union` with a name.
- **A type system that can state the laws.** Associative, commutative,
  idempotent is the entire correctness argument for a state-based CRDT, and
  Turmeric can put those three laws in a typeclass, attach them to
  `#refine{...}` contracts, and check them with a seeded fuzzer -- rather
  than leaving them in a doc comment, which is where nearly every CRDT
  library leaves them.

This plan proposes `spices/crdt`: a lattice core, the standard primitive
set (counters, sets, registers, maps), delta-state mutators carried on an
**associated type**, and a law-checking harness that is the reason to write
this in Turmeric rather than port it. Sequences (RGA) and the network sync
layer are explicitly separate, later phases.

## 1. What exists today, and the three real gaps

### 1.1 Substrate that already works

| Need | What we have | File |
| --- | --- | --- |
| Immutable state, cheap joins | persistent HAMT, structural sharing | `stdlib/hamt.tur` |
| Grow-only set join | `set-union`, `set-intersect`, `set-diff` | `stdlib/set.tur:225,262,301` |
| Keyed state | `Map K V` over the same HAMT | `stdlib/map.tur` |
| Replica identity | interned `Sym`, `sym=?` | `stdlib/sym.tur` |
| Per-CRDT delta type | associated types, incl. method return position | `docs/guides/typeclass-guide.md` |
| Deterministic clock for tests | `Mock-Time`, `mock-time-set`, `mock-time-get` | `stdlib/time.tur:168,201,219` |
| Seeded generation for the fuzzer | `Seeded-Random`, `seeded-next-int!` | `stdlib/random.tur:138,153` |
| Wire format | `derive-json` (defstruct), `derive-json-sum` (defdata) | `spices/json` |
| Equality / hashing for keys | `Eq`, `Hash`, `MapKey` | `stdlib/typeclass-eq.tur`, `typeclass-hash.tur`, `map.tur:370` |

Two facts about associated types are load-bearing below and are already
covered by fixtures, so the design is not speculating:

- A class method may **return** its own associated type, referenced bare
  inside the class body (`(sop-get [...] : Elem)`) --
  `tests/fixtures/typeclass-assoc-type-method-return`.
- A projection resolves per instance at annotation sites, including over
  multiple class parameters --
  `tests/fixtures/assoc-type-multiparam-projection`.

### 1.2 Gap 1 -- `map-merge` is right-biased, not value-combining

`stdlib/map.tur:681` documents its second argument as "wins on collision".
That is the wrong merge for every CRDT map: joining two `ORMap`s must join
the *values* under a shared key, not pick one. There is no `map-merge-with`
anywhere in stdlib.

This is the single most reusable thing the plan needs and does not have.
The resolution is in section 5: the spice ships it, over `map-iter`, rather
than growing stdlib.

The smaller half of the same gap: there is no `max` or `min` in stdlib at
all. `Ord` (`stdlib/typeclass.tur:24`) declares only `lt?` / `lte?` / `gt?` /
`gte?`, so even a G-Counter's pointwise join has to define its own. That is a
two-line function, and it is also the clearest measure of how absent the
lattice vocabulary is: the join of two integers under the natural order does
not have a name yet.

### 1.3 Gap 2 -- no monotonic clock, and `get-time-ms` is wall time

`stdlib/time.tur:237` exposes `get-time-ms`. Wall time is not an ordering:
it goes backwards under NTP, and two replicas disagree by more than the
message latency that would have ordered them causally. An LWW register
keyed on it silently loses writes.

The spice must ship a **hybrid logical clock** (HLC) rather than reach for
the wall clock, and the LWW types must take the stamp as a parameter so the
tests can drive them from `Mock-Time` deterministically.

### 1.4 Gap 3 -- replica IDs need real entropy, and `random.tur` is `rand()`

`stdlib/random.tur` is libc `rand()` seeded with `time(NULL)` (this is
already documented as the motivating defect in
[secret-spice-plan.md](secret-spice-plan.md) section 2.3). Two replicas
started in the same second can draw the same ID, and a duplicated replica
ID breaks a G-Counter's *arithmetic*, not just its tidiness -- two replicas
sharing a slot overwrite each other's counts under a pointwise `max`.

Resolution: `ReplicaId` is caller-supplied, and the doc page says so
plainly. A convenience constructor draws from `crypto-random-bytes!` **if**
the secret spice exists; otherwise there is no default and the caller
passes a name. Never silently `rand()`.

## 2. Design

### 2.1 The lattice core

```turmeric
;;; A join-semilattice: `join` is associative, commutative, and idempotent.
(defclass JoinSemilattice [a]
  (join [x : a y : a] : a))

;;; The identity element for `join`.
(defclass BoundedJoinSemilattice [a]
  (bottom [] : a))
```

Two flat classes, not one with a superclass: `defclass` has no `extends`
(no superclass support exists in `src/compiler/`, and the typeclass guide
has no section for it), so a function that needs both lists both. That is
the whole cost, and it is visible:

```turmeric
;;; The partial order induced by the join: x <= y iff (join x y) = y.
(defn lattice-leq? [^JoinSemilattice A ^Eq A] [x : A y : A] : bool
  (eq? (join x y) y))

;;; Fold a batch of states into one. Order-independent by the class laws.
(defn join-all [^JoinSemilattice A ^BoundedJoinSemilattice A]
               [xs : (Vec A)] : A
  ...)
```

`join-all`'s signature is the argument for the whole class: it is correct
for *every* instance, and the only reason it is correct is the three laws.
Section 3 is about making that claim checkable instead of aspirational.

### 2.2 Identity and causality

```turmeric
;; An interned replica name. Caller-supplied -- see 1.4.
(defopaque ReplicaId :Sym)

;; One replica's contribution: (replica, counter). The unit of causality.
(defstruct Dot [replica : ReplicaId  counter : int])

;; A version vector: the causal history a replica has observed.
(defstruct DotContext [seen : (Map ReplicaId int)])

;; A hybrid logical clock stamp. Total order, causally consistent, and
;; bounded-drift against wall time.
(defstruct Hlc [wall : int  logical : int  replica : ReplicaId])
```

`Dot` and `Hlc` carrying a `Sym`-backed field is newly possible: `Sym`
became a legal `defstruct`/`defdata` field type in **v0.46.1** (`defstruct
/defdata: `Sym` is a field type`, commit `4ff9ad724`). Before that release
`(defstruct Dot [replica : Sym ...])` was `defdata: field has unrecognized
type :Sym` and this record could not be written at all. `defopaque` **over**
`Sym` is the one shape here that is not yet exercised by a fixture; see
section 7.

Per the CLAUDE.md strict rule, none of these are `:int`. A `Dot` is not "a
packed 64-bit thing"; a `ReplicaId` is not "an index". The counter inside
`Dot` is a genuine machine integer and stays `:int`.

### 2.3 The primitive set

| Type | State | Join | Phase |
| --- | --- | --- | --- |
| `GCounter` | `(Map ReplicaId int)` | pointwise `max` | C1 |
| `PNCounter` | two `GCounter`s | pointwise, per side | C1 |
| `GSet A` | `(Set A)` | `set-union` | C2 |
| `TwoPSet A` | `(Set A)` adds + `(Set A)` tombstones | per side | C2 |
| `ORSet A` | `(Map A (Set Dot))` + `DotContext` | add-wins, dot-aware | C2 |
| `LwwRegister A` | `A` + `Hlc` | larger stamp, `ReplicaId` breaks ties | C3 |
| `MvRegister A` | `(Set A)` + `DotContext` | keep concurrent values | C3 |
| `ORMap K V` | `(Map K V)` + `DotContext` | recursive join on `V` | C3 |

```turmeric
(defstruct GCounter [entries : (Map ReplicaId int)])
(defstruct PNCounter [pos : GCounter  neg : GCounter])
(defstruct LwwRegister [A] [value : A  stamp : Hlc])

;; `ord-max` is the spice's own -- see 1.2. Two lines, no stdlib equivalent.
(defn ord-max [^Ord A] [x : A y : A] : A (if (gte? x y) x y))

(definstance JoinSemilattice [GCounter]
  (join [x y]
    (GCounter (map-merge-with ord-max (.entries x) (.entries y)))))

;;; The counter's reading. NOT part of the lattice -- it is the query.
(defn gcounter-value [c : GCounter] : int ...)
```

`ORMap`'s instance is the one that needs the whole apparatus at once: its
join is `map-merge-with` where the combining function is `join` at the
*value's* instance, so `(ORMap K V)` is a `JoinSemilattice` exactly when `V`
is. That is a constrained instance
(`docs/guides/typeclass-guide.md` section "Parametric Instances and
Constraints"), and it is what makes nesting work -- an `ORMap` of
`PNCounter`s merges correctly with no code specific to that pairing.

### 2.4 Delta-state, carried on an associated type

Shipping full state on every change is what makes naive CvRDT unusable over
a network. The delta-state formulation (Almeida, Shoker, Baquero) has each
mutator return a small value from the *same* lattice, which the receiver
joins. Turmeric can type that channel instead of erasing it:

```turmeric
(defclass DeltaCRDT [a]
  (type Delta : Type)
  (delta-bottom [] : Delta)
  (delta-join   [d : Delta e : Delta] : Delta)
  (apply-delta  [x : a d : Delta] : a))
```

`Delta` is referenced bare in method position inside the class body, which
is the shape `typeclass-assoc-type-method-return` already pins, and projects
as `(Delta GCounter)` at annotation sites outside it.

For a `GCounter` the delta is another `GCounter` holding one entry, so
`(type Delta = GCounter)`. For an `ORSet` it is a dot-store fragment plus
the causal context those dots were observed in -- a genuinely different type
from the set itself, which is exactly why the delta type must be per
instance rather than `a` itself. Most implementations either force
`Delta = a` (and lose the compression for causal types) or erase the
delta to bytes at the boundary. The associated type gives both.

Delta buffering (what to keep, when a peer's acknowledgment lets you drop
it) is a sync-layer concern and lands in C6, not here.

### 2.5 Purity, ownership, and regions

Every `join` is a pure function of two states: `#fx{}`, no I/O, no globals.
That is worth stating because it buys three things at once -- the law
fuzzer can call joins in any order, `with-region` can wrap a merge burst,
and the whole core stays runnable under `tur --interpret`.

The interpreter point has a concrete design consequence. `tests/run-turi.sh`
PASS-skips any fixture whose program contains a user inline-C block, so
**every inline-C body in the core costs interpreter coverage.** The lattice
core, the counters, the sets, and the registers should be written in plain
Turmeric over the existing HAMT natives, and inline-C reserved for
`map-merge-with`'s iterator walk if the Turmeric-level version measures
badly. This is the opposite of the secret spice, where the guarantees *are*
the inline C.

On regions: the spice introduces no new store primitive, so it adds no
`TUR_REGION_NOTE` obligations -- it writes through `map-assoc`, `set-add`
and the HAMT setters, which are already on the hooked list in CLAUDE.md. If
a later phase adds a mutable dot-store for performance, that changes and the
region-hook rule applies in the same commit.

Ownership follows the existing HAMT convention: joins return a new root and
leave both inputs valid. `map-free` / `map-free-all`'s split (wrapper vs.
contents) is the trap to document per type, since a `GCounter`'s entries are
`int` values but an `ORMap`'s are themselves CRDTs that own HAMTs.

No `:linear`. It is tempting -- the sqlite and secret spices both use it --
but a CRDT state is *meant* to be copied, shipped, and joined from many
places at once. Linearity would fight the data type. A `Replica` handle
owning a transport socket in C6 is a different story and may well want it.

### 2.6 Serialization

`derive-json` (defstruct) and `derive-json-sum` (defdata) from `spices/json`
cover the wire format with no new encoder, and give a readable format for
debugging a divergence, which is most of what you want early. The `:spices`
dep is `:optional true` in the manifest, matching how `spices/json` itself
depends on `spices/test` -- the core must build without a transport.

Binary framing belongs to
[hold/msgpack-spice-plan.md](hold/msgpack-spice-plan.md) when that lands;
CRDT deltas are a good forcing case for it and a bad reason to block on it.

## 3. Law checking -- the part that is not a port

A CRDT is correct if and only if its join is associative, commutative, and
idempotent, and its query is monotone in the lattice order. Every CRDT
library asserts this in prose. Turmeric can do better, in three layers of
increasing cost:

**Layer 1 -- the laws as contracts.** `#refine{...}` contract types are
always-on (the experiments registry is empty; see
`docs/guides/contract-types-guide.md`), so an instance can carry a checked
post-condition on its join in debug builds. Note the Release caveat from the
project's own experience: a Release-built `tur` compiles contracts out under
`NDEBUG`, so law checking is a Debug-build activity and the suite must say so
(this is the same trap recorded for the refine fixtures).

**Layer 2 -- a law harness.** One function per law, generic over the class:

```turmeric
(defn law-associative? [^JoinSemilattice A ^Eq A] [x : A y : A z : A] : bool
  (eq? (join (join x y) z) (join x (join y z))))

(defn law-commutative? [^JoinSemilattice A ^Eq A] [x : A y : A] : bool
  (eq? (join x y) (join y x)))

(defn law-idempotent? [^JoinSemilattice A ^Eq A] [x : A] : bool
  (eq? (join x x) x))
```

These are written once and instantiate for every type in the table. That is
the payoff for the typeclass: a new CRDT gets its correctness suite by
declaring an instance.

**Layer 3 -- a seeded differential fuzzer.** `spices/test` is `describe`/`it`
with no generators and no shrinking, so the harness ships its own generator
over `Seeded-Random`. The model is `tests/saffron-fuzz-src.py`, the in-repo
precedent for randomized differential testing: generate a random set of
per-replica operation sequences, apply them in N random delivery orders
(including duplicates and reorderings, which is the entire point), and
assert every replica reaches the same state. A failing seed is the repro.

Convergence under arbitrary delivery order is the property users actually
care about, and it is not implied by the three laws alone for the causal
types -- `ORSet` needs its dot context to be correct too. Layer 3 is what
catches that; layers 1 and 2 will not.

## 4. Back-end and dialect caveats

- **Interpreter:** the core is designed to run there (section 2.5). This is
  a real advantage over the secret spice and should be protected by keeping
  inline-C out of the primitives, not merely hoped for.
- **JIT:** nothing here is JIT-specific, but the corpus is a good stress
  case for the engine -- deeply nested generic joins over parametric records
  are exactly the shape that surfaced `jit-ffi-interp-refuses-parametric-
  record-field` and the `any` -> scalar miscompile fixed in v0.46.1. Expect
  to find at least one, and file it rather than working around it.
- **Saffron:** CRDT values through `any` are out of scope. The types are the
  point.

## 5. Why a spice, and why `map-merge-with` does not go in stdlib

The stdlib-growth gate is that additions are justified by tur-signal's
actual call surface. tur-signal does not merge replicated state, so
`JoinSemilattice`, `Dot`, and the primitives are spice material without
argument.

`map-merge-with` is the genuinely arguable one. It is a general-purpose
combinator, it is the obvious companion to an existing right-biased
`map-merge`, and every caller who has ever wanted "merge, but combine the
values" has had to walk the iterators by hand. The plan still says **spice**,
for one reason: putting it in stdlib means committing to its ownership
semantics for owned value types (who retains the combined box) at a moment
when no stdlib caller exists to pin them down. Ship it in `crdt/lattice`,
let the CRDT primitives be its first real users, and promote it to stdlib
once the ownership question has been answered by something other than a
guess. If it does get promoted, `map-merge`'s docstring should gain a
cross-reference, since discovering the right-biased one first and assuming
it combines is a live foot-gun today.

One stdlib-adjacent fix is worth doing regardless of this plan's fate:
`get-time-ms`'s docstring should say it is wall time and is not an ordering
(section 1.3). One comment block, no behavior change.

## 6. Phases

- **C1 -- lattice core + counters.** `JoinSemilattice`,
  `BoundedJoinSemilattice`, `lattice-leq?`, `join-all`, `map-merge-with`,
  `GCounter`, `PNCounter`, and law layers 1-2. Spice skeleton and manifest.
  Deliverable check: `join-all` over a `Vec` of `GCounter`s, and the three
  law functions instantiated for both counters.
- **C2 -- causal core + sets.** `ReplicaId`, `Dot`, `DotContext`, `GSet`,
  `TwoPSet`, `ORSet`. Law layer 3 (the fuzzer) lands here, because `ORSet`
  is the first type the laws alone do not cover.
- **C3 -- registers + maps.** `Hlc`, `LwwRegister`, `MvRegister`, `ORMap`
  with its constrained recursive instance. Deterministic LWW tests driven
  from `Mock-Time`.
- **C4 -- deltas.** `DeltaCRDT`, per-instance `Delta` bindings, delta
  mutators for every C1-C3 type, and the equivalence test that matters:
  joining a sequence of deltas equals joining the full states.
- **C5 -- sequences.** RGA (or Fugue) for replicated text. This is the hard
  one -- interior ordering, tombstones, and an index-to-position map -- and
  it is the one that most deserves to be deferred until C1-C4 are boring.
- **C6 -- sync.** A separate `crdt-sync` spice over `valkey` pubsub or the
  `ws-*` spices: delta buffering, acknowledgment, anti-entropy. Separate
  because a transport dependency has no business in the core, and because
  the core is testable without one.

C1-C3 are the useful unit; someone can build with the spice at the end of
C3. C4 is what makes it usable over a network, C5 is a research-grade
follow-on, C6 is plumbing.

## 7. Risks and open questions

- **`defopaque` over `Sym` is unverified.** `(defopaque ReplicaId :Sym)` is
  the natural spelling, but the fixture coverage that landed in v0.46.1 is
  for `Sym` as a `defstruct`/`defdata` **field**, not as a `defopaque`
  representation. Probe it first; the fallback is a one-field
  `(defstruct ReplicaId [name : Sym])`, which is verified. Do not fall back
  to `:int`.
- **Tombstone growth is the standing CRDT complaint.** `TwoPSet` and `ORSet`
  grow without bound under churn. The honest answer is a documented
  compaction protocol (stable-causal-cut based) in C6, not a claim that it
  does not happen. The doc page should carry a "when not to use this" section
  naming it.
- **Constraint lists get long without superclasses.** `[^JoinSemilattice A
  ^BoundedJoinSemilattice A ^Eq A ^Hash A ^MapKey A]` is a realistic
  signature for an `ORMap` helper. Functional dependencies
  (`docs/guides/typeclass-guide.md`) may reduce some of it. If the
  ergonomics turn out to be the thing that kills adoption, that is a real
  data point for superclass support as a compiler feature -- file it as a
  report rather than fighting it in the spice.
- **`Eq` for law checking must be structural, not pointer identity.** The
  law functions are all `eq?` calls, and the existing helpers differ in
  whether they take one: `set-eq?` (`stdlib/set.tur:344`) takes no comparator
  while `set-eq-cmp?` (`:382`) does, and both `map-eq?` (`stdlib/map.tur:773`)
  and `map-eq-k?` (`:813`) require a value comparator. A law suite wired to a
  comparator-free variant would compare pointers and pass on everything. Pin
  this with a deliberately-failing instance in the C1 tests.
- **HLC needs a bounded-drift check.** An HLC whose wall component runs away
  from real time is a correctness problem that looks like nothing until
  restart. Ship the max-drift rejection with the clock, not after.
- **Nothing here is measured.** No benchmark exists for HAMT join cost at
  the sizes a real document hits. C1 should land a small benchmark alongside
  the correctness suite, so C5's design (which is where performance stops
  being free) has a baseline to argue against.

## 8. References

- Shapiro, Preguica, Baquero, Zawirski, *A comprehensive study of
  Convergent and Commutative Replicated Data Types* (INRIA RR-7506, 2011)
  -- the primitive catalog in section 2.3.
- Almeida, Shoker, Baquero, *Delta State Replicated Data Types* (2016) --
  the delta formulation in section 2.4.
- Kulkarni et al., *Logical Physical Clocks* (2014) -- the HLC in 2.2.
- Weiss, Urso, Molli, *Logoot*; Roh et al., *RGA* -- candidates for C5.
- In-tree: [secret-spice-plan.md](secret-spice-plan.md) (CSPRNG, and the
  template this plan follows), `docs/guides/typeclass-guide.md`
  (associated types, constrained instances),
  `docs/guides/contract-types-guide.md` (`#refine{...}`).
