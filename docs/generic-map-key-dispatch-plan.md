# Generic Map Key Dispatch -- `hamt-of` / `#map{...}` over typed keys (GMK0--GMK4)

> **Status:** Not started. Follow-up to TCE4 (see
> [typed-collection-elements-plan.md](typed-collection-elements-plan.md#tce4--polymorphic-map-keys-via-hashkeqk)).
>
> **Type:** Compiler + stdlib. No new runtime: the content-equality primitive
> already exists (`tur_hamt_*_eq` + the thread-local `keys_equal` override,
> landed in TCE4).
>
> **One-line goal:** writing `#map{"name" 1}` (or `(hamt-of "a" 1 "b" 2)`)
> should produce a *correct content-keyed* map -- where two distinct string
> pointers with equal text are the same key -- instead of today's
> hash-as-key normalization (which conflates hash-colliding keys and breaks
> on distinct-pointer equal strings).

## Background -- what TCE4 left in place

TCE4 made content-keyed maps **possible** but not **automatic**:

- The runtime HAMT gained `tur_hamt_{set,del,has,get}_eq`, which consult a
  thread-local key-equality override (`src/runtime/hamt.c`). `eq == NULL`
  reproduces the old identity behaviour exactly.
- `stdlib/map.tur` exposes `map-{assoc,get,has?,dissoc}-eq` (explicit hash +
  key-equality closure) and an `smap-*` convenience layer hard-wired for
  `:cstr` keys (`cstr-hash` + `tur-cstr-key-eq?`).

What did **not** change: `hamt-of` and the `#map{...}` data literal still
expand onto the identity-keyed `map-assoc`. For `:cstr` keys they fall back
to the historical behaviour -- the `#map{...}` reader normalizes a string or
keyword key to its content hash and stores *the hash* as the key, comparing
hashes by identity. That "works" for distinct string literals only by luck
of hash uniqueness; two equal-content strings at different addresses, or two
distinct keys that hash-collide, are handled incorrectly.

The aim of this plan is to retire that fallback for typed scalar keys by
dispatching on the static key type at the `hamt-of` / `#map{...}` boundary.

## Building blocks that already exist

These were verified in the codebase (2026-05-31):

- **`Hash` typeclass** -- `stdlib/typeclass.tur`:
  `(defclass Hash [a] (hash [x] :int))` with instances for `int` (identity),
  `bool`, `cstr` (content hash via `tur_hamt_hash_str`), and `float32`.
- **`Eq` typeclass** -- `stdlib/typeclass-eq.tur`:
  `(defclass Eq [a] (eq? [x y] :bool))` with instances for `str`, `int`
  (builtin `=`), and the structural types.
- **TCE4 runtime + stdlib eq layer** -- `tur_hamt_*_eq`, `map-*-eq`,
  `smap-*`, `cstr-hash`, `tur-cstr-key-eq?`.

So this is a *wiring* problem (route `hamt-of` through `Hash[K]` + `Eq[K]`
into the `*-eq` ops), not a from-scratch typeclass build. The note in the
TCE4 section that this "needs a real Hash typeclass" is superseded by this
discovery -- the class exists; what is missing is reliable dispatch and a
way to thread the per-type equality into the runtime call.

## Known wrinkles (must be resolved first)

Two small probes show the wiring is not yet free:

1. **`Hash` dispatch for `:cstr` miscompiles today.** A bare `(hash "hi")`
   in a fresh file emits a reference to an undeclared `hash_<gensym>`
   (`'hash_758' undeclared`), i.e. the `Hash[cstr]` instance method is not
   being emitted/linked when invoked generically. `(hash 5)` (int) works.
   Until generic `Hash` resolution is solid for `:cstr`, no dispatch path
   built on `(hash key)` will compile. This is GMK0.
2. **Passing a class method as a first-class value is not resolved.**
   Referencing `eq?` as a value (to hand to `map-assoc-eq` as the comparator
   closure) fails name resolution (`TUR-E0003`). Threading `Eq[K]` into the
   runtime call therefore needs either method-as-value support, or a
   per-type wrapper `defn` whose address is taken (the mechanism the TCE4
   `smap-*` layer uses with `tur-cstr-key-eq?`).

## Approaches

### Approach A -- constrained-generic stdlib dispatch (preferred if feasible)

Add a single generic that the macros target:

```turmeric
(defn map-assoc-g [K V] #{(Hash K) (Eq K)} [m :int key :K val :V] :int
  (map-assoc-eq m (hash key) key val <eq-closure-for-K>))
```

`hamt-of` / `#map{...}` then expand onto `map-assoc-g` (and `map-get-g`,
etc.), and dispatch falls out of the `Hash[K]` / `Eq[K]` constraints at each
concrete instantiation. The open question is `<eq-closure-for-K>`: the
constraint provides the `Eq[K]` dictionary, but the comparator must reach
the runtime as a plain `bool(int64_t,int64_t)` function pointer. Sub-options:

- **A1:** make typeclass methods referenceable as first-class values, so the
  dictionary-resolved `eq?` can be passed directly (general fix, larger).
- **A2:** synthesize, per concrete `K`, a tiny non-capturing wrapper
  `defn __eq_K [a :K b :K] :bool (eq? a b)` and pass its address -- mirrors
  the hand-written `tur-cstr-key-eq?`. Could be emitted by the compiler at
  the dispatch site, or provided per-instance.

Pros: keys of *any* `Hash`/`Eq` scalar type work uniformly; no key-type
special-casing in the lowering. Cons: depends on GMK0 + method-as-value (or
wrapper synthesis); int keys must stay on the zero-overhead identity path,
so `map-assoc-g` needs `Hash[int]`/`Eq[int]` to lower to exactly today's
code (no closure call) -- likely needs the `eq == NULL` fast path.

### Approach B -- compiler-level lowering dispatch (robust fallback)

At `#map{...}` / `hamt-of` elaboration, inspect the *static types* of the
key expressions and pick the lowering:

- all keys `:int` (or `:bool`) -> existing identity `map-assoc` chain
  (unchanged, zero overhead, no snapshot churn);
- all keys `:cstr` -> `smap-assoc` chain (content equality);
- mixed key types -> the existing homogeneity error (TCE3) already rejects
  this, so the lowering only ever sees one key type.

Pros: no dependence on generic method dispatch; reuses the already-correct
`smap-*` and identity paths; smallest surface in the type system. Cons:
key-type knowledge must be available where the literal lowers (`elab` has
types; the reader does not -- so the dispatch belongs in `elab_toplevel.c` /
`forms.c`, not `reader_macros.c`); each new scalar key type needs a branch.

### Approach C -- hybrid

Use Approach B's lowering switch as the user-facing dispatch, but have the
`:cstr` (and future) branch target Approach A's `map-assoc-g` once GMK0 and
the eq-closure question are solved, collapsing the special-casing over time.

## Phasing

### GMK0 -- fix generic `Hash` resolution (prerequisite)

Make `(hash x)` resolve and compile for `:cstr` (and the other declared
instances) in a generic context. Root-cause the `hash_<gensym> undeclared`
emission (instance method not emitted when used generically). Acceptance: a
fixture computing `(hash "a")`, `(hash 1)`, `(hash true)` builds and prints
stable values; equal strings hash equally.

### GMK1 -- decide the eq-threading mechanism

Resolve the `<eq-closure-for-K>` question: implement method-as-value (A1) or
per-type wrapper synthesis (A2), or commit to Approach B and skip. Produce a
short decision note appended to this plan. Acceptance: a generic
`map-assoc-g` (or the B lowering) round-trips `:cstr` keys with
distinct-pointer equality, reusing the TCE4 `tce4-map-cstr-key` assertions.

### GMK2 -- route `hamt-of` / `#map{...}` to the dispatched path

Wire the chosen approach into the literal lowering. Keep the `:int` path
byte-for-byte identical (verify zero `expected.c` churn for int-keyed
fixtures). Remove the hash-as-key normalization for `:cstr` keys (or make it
a no-op now that real content keys are stored). Acceptance: `#map{"a" 1
"b" 2}` and `(hamt-of "a" 1)` build content-keyed maps; `data-literal-map-*`
fixtures still pass; a new `gmk-map-literal-cstr-key` fixture proves
distinct-pointer equality through the literal surface.

### GMK3 -- generalize reads and the rest of the map surface

Ensure `map-get` / `map-has?` / `map-dissoc` (and `map-merge`, iteration,
`Eq [Map]`) work for content keys -- today they assume identity. Provide the
`-g` (or `smap-`) equivalents wherever the literal can construct a
content-keyed map, so lookups use the same `Eq[K]`. Acceptance: get/has/
dissoc/merge round-trip on a `#map{...}`-built string map.

### GMK4 -- docs + snapshot regen

Update `docs/guides/data-literals-guide.md` (the "Element types" section
already notes keys are int-only -- relax it), the `hamt-of` docstring, and
the TCE4 follow-up note. Regenerate `docs/api/` + `stdlib/docstrings.tur`
and all `expected.c` snapshots. Acceptance: `bash tests/run.sh` zero `FAIL`.

## File touchpoints

| Phase | File | Change |
|---|---|---|
| GMK0 | `src/compiler/` (instance method emission) + `stdlib/typeclass.tur` | Fix generic `Hash[cstr]` resolution/emission |
| GMK1 | compiler (method-as-value) **or** `stdlib/map.tur` | Eq-closure threading mechanism |
| GMK2 | `src/compiler/elab_toplevel.c`, `forms.c` (+ `reader_macros.c` if dispatch must move) ; `stdlib/map.tur` | Key-type dispatch for `hamt-of` / `#map{...}` |
| GMK3 | `stdlib/map.tur` | Content-key variants across the map surface |
| GMK4 | `docs/`, snapshots | Docs + regen |

## Non-goals

- Aggregate (multi-word struct/ADT) keys -- the carrier slot stores one
  word; this plan covers scalar `Hash`/`Eq` key types only.
- Changing value typing (TCE3 already covers scalar values).
- Custom user `Hash`/`Eq` instances for new key types -- should work for
  free once dispatch is generic (Approach A), but is not a release gate.

## Risks

- **Int-key regression / snapshot churn.** The `:int` path must stay
  identical. Gate every phase on zero `expected.c` diff for int-keyed
  fixtures (only new stdlib bodies may appear in the preamble, as in TCE4).
- **Generic dispatch depth (GMK0).** The `hash_<gensym> undeclared` bug may
  point at a broader instance-method-emission gap; scope it before
  committing to Approach A.
- **Key lifetime.** The HAMT does not own keys (TCE4 caveat). Content-keyed
  literals built from string literals are fine (static storage); document
  that runtime-built keys must outlive the map, as the `smap-*` docs already
  note.
