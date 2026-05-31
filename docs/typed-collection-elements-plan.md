# Typed Collection Elements -- Polymorphic Vec/Map carriers (TCE0--TCE6)

> **Status:** Not started.
>
> **Type:** Compiler + stdlib (no new runtime; the existing `int64_t`
> carrier buffer is reused).
>
> **Approach:** "Approach B" from the vec/map element-type investigation --
> make the `Vec[A]` / `Map[K V]` *operations* polymorphic over a scalar
> element type and have the compiler bridge each instantiation across the
> existing `int64_t` carrier with a per-instance reinterpret. This is the
> lighter-weight alternative to full layout monomorphization (TS3b).
>
> **Depends on:**
> - [archive/route-b-typed-slots-plan.md](archive/route-b-typed-slots-plan.md)
>   -- TS1 typed thunks, TS2 `EX_REINTERPRET`, TS3 typed containers
>   (Cons/Option/Pair/Result) are all landed and are the template here.
> - [archive/history/typed-slots-gs5-compiler-support-plan.md](archive/history/typed-slots-gs5-compiler-support-plan.md)
>   -- the generic-substrate / scalar-boundary work this plan extends.
>
> **Relationship to TS3b:** `route-b-typed-slots-plan.md` deferred `Vec`
> to a sub-phase TS3b that rewrites all eight inline-C functions to
> `sizeof(A)` / `A*` indexing (a real `double*` buffer). TCE is a smaller
> intermediate: it keeps the single `int64_t` buffer and bridges *scalar*
> element types by bit-reinterpret. TCE unblocks `:cstr`/`:bool`/`:nil`/
> `:float` element checking now; TS3b remains the path for genuinely
> aggregate (multi-word struct/ADT) elements. See "Approach B vs TS3b".
>
> **Last updated:** 2026-05-31

---

## Motivation

`vec-of` / `hamt-of` (and their underlying `vec-push!` / `map-assoc`)
declare their element parameters as a literal `:int`:

```turmeric
(defn vec-push! [v :int val :int] :nil ...)        ; stdlib/vec.tur:85
(defn map-assoc [m :int key :int val :int] :int ...) ; stdlib/map.tur:64
```

The `Vec [A]` (`vec.tur:12`) and `Map [K V]` (`map.tur:14`) type parameters
live only on the *structs*; they are phantom and never threaded into the
operations. The checker compares argument kinds by strict `TypeKind`
equality (`src/compiler/elab_call.c:1738`), so:

```turmeric
(vec-of "x")    ; TUR-E0001: function 'vec-push!' arg 2: expected int, got cstr
(vec-of 1.5)    ; TUR-E0001: function 'vec-push!' arg 2: expected int, got float
```

`[...]` lowers to `vec-of` and `#map{...}` to `hamt-of`, so the
data-literal forms inherit the same restriction. Yet `:cstr`, `:bool`,
`:nil`, and `:float` all fit the carrier: pointers and `:bool`/`:nil` are
already `int64_t`-width, and a `:float` is a `double` (8 bytes = `int64_t`).
The rejection is a missing-polymorphism limitation, not a representational
one.

The sibling containers already solved this. `Cons[A]`, `Option[A]`,
`Pair[A B]`, and `Result[A B]` are parameterized over a real element type
and monomorphize to concrete C layouts (`Cons__float { double head; ... }`,
verified in emitted C). Vec and Map are the documented hold-outs.

---

## Empirical baseline (measured 2026-05-31)

These probes define exactly what works today and what the compiler must
gain. Reproduce with `./build/tur run <file>`.

1. **Concrete-typed wrappers work end to end.** A hand-written
   monomorphic wrapper that reinterprets a known scalar through the
   carrier round-trips losslessly:

   ```turmeric
   (defn vec-push-f! [v :int x :float] :nil (vec-push! v (:: x :int)))
   (defn vec-get-f  [v :int i :int] :float (:: (vec-get v i) :float))
   ;; prints 1.5, 2.25 correctly
   ```

   The same pattern works for `:cstr`. This proves the carrier is
   representationally sufficient -- only the *type surface* is missing.

2. **A bare tyvar into an `:int` param is rejected.**

   ```turmeric
   (defn vpush! [A] [v :int x :A] :nil (vec-push! v x))
   ;; TUR-E0001: function 'vec-push!' arg 2: expected int, got tyvar
   ```

3. **An explicit `::` reinterpret on a tyvar type-checks but
   miscompiles.** With `(vec-push! v (:: x :int))` in a `[A]` body, the
   call passes `tur check`, but the monomorphized specialization drops the
   reinterpret:

   ```c
   static void vpush___spec__void_int64_t_double(int64_t v, double x) {
       vec_push_(v, x);   /* double truncated into int64_t param -- no bitcast */
   }
   ```

   The `EX_REINTERPRET` node produced by `::` on a tyvar does not survive
   specialization once `A` is bound to a concrete scalar, so the value is
   passed by C numeric conversion (truncation for `:float`, pointer-to-int
   warning + crash for `:cstr`).

**Conclusion.** The single blocking gap is (3): the compiler must preserve
(or re-insert) a per-instance size-equal reinterpret when a scalar generic
value crosses the `int64_t` carrier boundary -- in the argument-into-carrier
direction and the carrier-result-to-typed direction. Everything else (TS2
codegen, struct-app monomorphization, typed thunks) is already in place.

---

## Goals and non-goals

### Goals

1. `(vec-of e ...)`, `[e ...]`, `(hamt-of k v ...)`, and `#map{k v ...}`
   type-check and run for element types `:int`, `:cstr`, `:bool`, `:nil`,
   and `:float`, with the element type inferred and enforced (all elements
   must unify to one `A`).
2. `vec-get` / `map-get` return the element at its real static type, so
   downstream code needs no manual `::`.
3. Preserve today's homogeneity guarantee: mixed-type literals stay a
   `TUR-E0001` on the offending element.
4. Keep the change additive -- the existing monomorphic `int64_t` ABI of
   the underlying primitives is untouched; only a polymorphic surface is
   layered on top.

### Non-goals

- Full `A*` / `sizeof(A)` buffer layout (native `double[]` storage). That
  is TS3b; TCE deliberately keeps the `int64_t` buffer and bridges by
  reinterpret.
- Aggregate (multi-word struct / ADT) element types in Vec/Map. A
  16-byte struct does not fit the 8-byte carrier; those need TS3b's real
  per-element layout. TCE restricts polymorphism to *scalar* element types
  (`type_size_bytes(A) <= 8` and a single-word ABI).
- Changing `*args*` or any externally depended-on ABI.
- Removing the explicit-hash `map-assoc-h` / `map-get` primitives; they
  remain the low-level entry points.

---

## Approach B vs TS3b -- when to pick which

| | TCE (Approach B) | TS3b (full monomorphization) |
|---|---|---|
| Buffer layout | one shared `int64_t[]`, reinterpret at edges | per-`A` `A*` buffer (`double[]`, ...) |
| stdlib churn | thin polymorphic surface over existing inline-C | rewrite all 8 `vec.tur` fns + HAMT carrier |
| Scalar elements (`:cstr/:bool/:nil/:float`) | supported | supported |
| Aggregate elements (multi-word structs/ADTs) | not supported | supported |
| `:float` storage | bit-reinterpret through `int64_t` | native `double` slot |
| Compiler work | one boundary-reinterpret fix (the TCE0 gap) | layout specialization for Vec + HAMT |

TCE is the right first step: it closes the user-visible fact with a single
focused compiler fix plus a stdlib surface, and it leaves TS3b as a clean
follow-on for aggregate elements without re-doing any of this work.

---

## Phase plan

Each phase leaves the tree green (`bash tests/run.sh` with zero `FAIL`)
and regenerates fixture snapshots per the CLAUDE.md codegen-snapshot rule.

### TCE0 -- Scalar generic carrier-boundary reinterpret (the enabler)

**Goal.** When a value whose static type is a generic `A` (instantiated to
a concrete same-size scalar) crosses into an `int64_t` carrier parameter,
or a carrier result is consumed at type `A`, the monomorphizer emits a TS2
`EX_REINTERPRET` in the *specialized* instance.

**Compiler changes.**

- `src/compiler/elab_call.c` (~1738) -- accept a scalar tyvar argument
  against an `:int` carrier parameter (and vice versa) by inserting an
  `EX_REINTERPRET` at the call boundary, rather than emitting
  `TUR-E0001 ... got tyvar`. Gate strictly on
  `type_size_bytes(A) == 8 || A` being a single-word scalar.
- Specialization path (the `*__spec__*` emission that backs
  `vpush___spec__...`) -- ensure an `EX_REINTERPRET` whose source is a
  now-concrete scalar tyvar is *preserved*, not folded away, when the
  instance is generated. This is the direct fix for empirical finding (3);
  today the node is dropped and the value is passed by C numeric
  conversion.
- Reuse the existing `EX_REINTERPRET` codegen
  (`src/compiler/emit_expr.c`, the size-equal union trick) unchanged.

**Acceptance.** `tests/fixtures/typed-slots/scalar-tyvar-carrier/` -- a
hand-written `(defn id-thru [A] [v :int x :A] :A ...)` that pushes `x`
into and reads it back from the carrier, asserted for `:float` (`1.5`
survives, not `1`) and `:cstr` (pointer survives, no crash). The emitted
specialization must contain a `union { ... }` bitcast, not a bare
`vec_push_(v, x)`.

> This phase is the whole compiler cost of Approach B. TCE1+ are stdlib +
> macro work that ride on it.

### TCE1 -- Polymorphic Vec operation surface

**Goal.** Element-typed `vec-push!` / `vec-get` / `vec-set!` / `vec-pop!`
without touching the inline-C primitives.

**Stdlib changes (`stdlib/vec.tur`).**

- Keep the existing inline-C functions as monomorphic `int64_t`
  primitives; rename to a `*-raw` suffix (e.g. `vec-push-raw!`,
  `vec-get-raw`) to make the carrier explicit. They remain `#{Unsafe}`-free
  and unchanged in body.
- Add the polymorphic public surface that bridges via TCE0:

  ```turmeric
  (defn vec-push! [A] [v :int val :A] :nil (vec-push-raw! v val))
  (defn vec-get  [A] [v :int i :int] :A   (vec-get-raw v i))
  (defn vec-set! [A] [v :int i :int val :A] :void (vec-set-raw! v i val))
  (defn vec-pop! [A] [v :int] :A          (vec-pop-raw! v))
  ```

  With TCE0 landed, the `val :A -> :int` and `:int -> :A` crossings emit
  the per-instance reinterpret automatically; no user `::` is required.
- Restrict `A` to scalar element types (see Non-goals). A non-scalar `A`
  produces a clear diagnostic pointing at TS3b.

**Acceptance.** `tests/fixtures/typed-slots/vec-float/`,
`tests/fixtures/typed-slots/vec-cstr/`,
`tests/fixtures/typed-slots/vec-bool/` -- push/get round-trips per type.

### TCE2 -- `vec-of` element inference + homogeneity check

**Goal.** `(vec-of a b c)` infers the unified element type `A` from its
arguments and instantiates the TCE1 ops at `A`; a mixed-type literal is a
`TUR-E0001` on the offending element (today's behaviour, preserved).

**Changes.**

- `stdlib/vec.tur` -- `vec-of` / `vec-push-each__` expand onto the
  polymorphic `vec-push!`. Because all pushes target the same `v`, the
  element type unifies through the shared `A`; a divergent element fails
  unification with `TUR-E0001` at that element's position.
- No macro-level type computation is needed -- unification falls out of
  the polymorphic `vec-push!` signature. Confirm the diagnostic still
  points at the offending element span (`vec-of 1 "x"` underlines `"x"`).

**Acceptance.** `tests/fixtures/typed-slots/vec-of-infer/` (homogeneous
float/cstr literals) and a negative fixture asserting
`(vec-of 1 "x" 3.14)` reports `TUR-E0001` on `"x"`.

### TCE3 -- Polymorphic Map value surface

**Goal.** `Map[K V]` values of scalar type `V` (`map-assoc` /
`map-get` / `map-dissoc`) without manual reinterpret.

**Stdlib changes (`stdlib/map.tur`).**

- As in TCE1: keep the inline-C HAMT bridges as `*-raw` `int64_t`
  primitives; add a polymorphic value surface:

  ```turmeric
  (defn map-assoc [K V] [m :int key :K val :V] :int ...)
  (defn map-get   [K V] [m :int h :int key :K] :V ...)
  ```

- Values bridge through TCE0 exactly like Vec elements.

**Keys** are handled in TCE4 (they need hashing/equality, not just
storage). For TCE3, keep keys on the `:int` carrier and the explicit-hash
primitives (`map-assoc-h`, `map-get` with precomputed `h`) unchanged.

**Acceptance.** `tests/fixtures/typed-slots/map-cstr-val/` (int keys,
cstr/float values).

### TCE4 -- Polymorphic Map keys via Hash[K]/Eq[K]

**Goal.** Scalar non-int keys (`:cstr` especially) without callers
precomputing the hash.

**Changes.**

- `stdlib/map.tur` already documents `Map[K V]` as "requiring Hash[K] and
  Eq[K]". Make the polymorphic `map-assoc` / `map-get` / `hamt-of` derive
  the hash from the `Hash[K]` instance (`stdlib/hash.tur`) and compare via
  `Eq[K]`, instead of treating the key bits as the hash.
- Keep the int-key fast path: for `K = :int` the identity hash matches
  today's behaviour, so existing int-keyed maps are unaffected.
- Constrain `K` to types with `Hash`/`Eq` instances; a missing instance is
  a typeclass-resolution error, not a silent bit-hash.

**Acceptance.** `tests/fixtures/typed-slots/map-cstr-key/` -- string keys
round-trip with collisions exercised.

### TCE5 -- Data-literal lowering

**Goal.** `[e ...]` and `#map{k v ...}` ride the typed surface
transparently.

**Changes.**

- `src/compiler/elab_toplevel.c` (~1066-1090) already lowers `[...]` to
  `vec-of` and `#map{...}` to `hamt-of`; no lowering change is needed once
  TCE2/TCE3/TCE4 land. Add fixtures that exercise the literal forms at
  non-int element types under `-Xdata-literals`.

**Acceptance.** `tests/fixtures/typed-slots/data-literal-cstr/`
(`["a" "b"]` and `#map{:k "v"}`).

### TCE6 -- Docs + snapshot regeneration

- Update the `vec-of` / `hamt-of` docstrings (remove "Carrier type is
  :int" / "Keys are int-valued" caveats; state the scalar-element rule and
  the TS3b boundary for aggregates).
- Note the new rule in
  [docs/guides/data-literals-guide.md](guides/data-literals-guide.md).
- Regenerate `docs/api/` (`just docs`) and all
  `tests/fixtures/*/expected.c` snapshots per the CLAUDE.md codegen rule;
  commit snapshots alongside the change.

---

## File change summary

| Phase | File | Change |
|---|---|---|
| TCE0 | `src/compiler/elab_call.c` | Accept scalar tyvar <-> `:int` carrier crossings; insert `EX_REINTERPRET` |
| TCE0 | specialization emit path | Preserve `EX_REINTERPRET` on concretized scalar tyvars |
| TCE1 | `stdlib/vec.tur` | `*-raw` primitives + polymorphic `[A]` op surface |
| TCE2 | `stdlib/vec.tur` | `vec-of` expands onto polymorphic `vec-push!` |
| TCE3 | `stdlib/map.tur` | `*-raw` HAMT bridges + polymorphic value surface |
| TCE4 | `stdlib/map.tur` | Hash[K]/Eq[K]-driven polymorphic keys; int fast path |
| TCE5 | (fixtures only) | Literal-form coverage; lowering already in place |
| TCE6 | docstrings, `docs/api/`, fixtures | Doc + snapshot regen |

---

## Test plan

- New fixtures under `tests/fixtures/typed-slots/` per phase (listed
  above). Each asserts both `tur run` output and, where the boundary
  matters, an emitted-C assertion (presence of the `union` reinterpret,
  absence of a bare truncating store).
- A negative fixture for mixed-element literals (`vec-of 1 "x"`) asserting
  `TUR-E0001` on the right span.
- Full `bash tests/run.sh` green at the end of every phase.
- `tests/fixtures/*/expected.c` snapshots regenerated and committed with
  the codegen-affecting phases (TCE0 changes call-site emission).

---

## Open design decisions

1. **`*-raw` naming vs an `#{Unsafe}` carrier module.** Splitting into
   `vec-push-raw!` exposes the carrier primitive in the public namespace.
   Alternative: keep the inline-C private and have the polymorphic surface
   inline the C via a fixed-arity helper. Leaning `*-raw` for clarity and
   because it mirrors the existing `map-assoc` / `map-assoc-h` split.
2. **Scalar restriction enforcement.** Where to reject a non-scalar `A` --
   in `elab_call.c` at the reinterpret-insertion site (uniform, early), vs
   a dedicated diagnostic on the vec/map signatures (clearer message). The
   message should name TS3b as the path for aggregate elements.
3. **`:float` key hashing (TCE4).** Hashing float bits is well-defined but
   `0.0`/`-0.0` and `NaN` need a documented policy; may restrict map keys
   to `:int`/`:cstr`/`:bool` initially and defer float keys.
4. **Interaction with the KB-015 specialization-cache bug** noted in
   `route-b-typed-slots-plan.md` (a spurious `int64<->double` reinterpret
   around a second generic-accessor call). TCE0 should verify it does not
   reintroduce or collide with that cache; add a regression probe if the
   typed vec/map accessors trip it.
