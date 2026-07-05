# Plan: By-Value Propagation for Composed van Laarhoven Lenses (Path B)

> **Status:** Proposed
> **Last Updated:** 2026-07-05
> **Type:** Compiler / Codegen
> **Predecessors:** van-laarhoven-monomorphization-plan.md (VBM1-VBM4),
> van-laarhoven-consumer-mono-plan.md (CM1-CM4)

## Goal

Bring **composed** van Laarhoven lenses onto the by-value monomorphization path
(Path B), removing the last correctness carve-out left by the CM4 graduation
(2026-07-05). A composed lens and any consumer that is ever passed one currently
fall fully back to the boxed Path A carrier bridge; this plan makes the whole
composed chain thread `(f a)` by value with no carrier box.

---

## Background: what graduated, and the one gap left open

CM4 made Path B unconditional (the `--enable=vl-wide-mono` flag is retired). The
empirical basis: every `van-laarhoven-lens-wide-*` fixture was run through Path B
and only **lens composition** (`van-laarhoven-lens-wide-compose`) mis-compiled.
Direct lenses, generic consumers (`view`/`over`/`set`), and receiver-reading
`fmap` instances all compile and run correctly by value.

A composed lens is one whose body **tails into another lens** rather than a
direct `fmap` dispatch. Compare:

```turmeric
;; SIMPLE -- body tail is a single fmap dict dispatch
(defn point-x [^f] [^Functor f g : (-> int (f int)) s : Point] : (f Point)
  (fmap (g (.x s))
        (fn [nx : int] : Point (make-struct Point :x nx :y (.y s)))))

;; COMPOSED -- body tail is a call to ANOTHER lens (line-a), via an adapter
(defn line-a-x [^f] [^Functor f g : (-> int (f int)) s : Line] : (f Line)
  (line-a (fn [p : Point] : (f Point) (point-x g p)) s))
```

The structural discriminator shipped in CM4 is `lens_is_simple_for_pathb`
(`src/compiler/mono_specs.c`): peel `ascribe`/`return`/`let`/`do` wrappers off the
lens body and check whether the tail is an `EX_CALL` carrying an `EX_DICT`
`dict_arg` whose method is `fmap`. Simple lenses pass; composed lenses do not and
are gated to Path A.

### The current gate (CM4, shipped)

- `lens_is_simple_for_pathb(const FnDef*)` classifies each concrete lens.
- In `resolve_walk`, a composed concrete lens is **not** registered (no concrete
  key, so VBM2b never emits an ill-typed `<lens>__mono` body) and its consumer
  key is **poisoned** (`has_composed_lens`).
- A poisoned consumer key makes every Path-B accessor
  (`mono_spec_lens_set_count`, `..._set_get`, `..._clone_hash`,
  `..._redirect_for_binding`, `..._consumer_call_lookup`) report "nothing," so
  the consumer -- including any *simple* lenses sharing it -- stays entirely on
  Path A. Poison propagates transitively through forwarded lens params.

This is conservative and correct, but it leaves composition paying the
box/alloc/copy/free per crossing, and it de-optimizes any consumer that mixes a
simple and a composed lens.

---

## Root cause (ASan-confirmed)

For `over line-a-x`, the wide consumer builds a **by-value** `g`: `over__spec`
sets `g.__fn = __fn_1335__spec`, which returns `Identity__int` **by value**. But
the composed lens's nested `point-x` lowered to the **carrier** form
`point_hyx_...`, which invokes `g` with a carrier cast
`((int64_t (*)(void*, int64_t))g[0])(...)`. That reads only the **first word** of
the by-value `Identity` struct (`700 = h(a)`) as if it were a boxed pointer and
feeds it to `fmap`, whose `run_id`/`run-id` dereferences `700` as a
`tur_adt_Identity *` -> SEGV.

In one line: **a composed wide lens straddles a by-value outer `g` and
carrier-lowered nested lenses (`point_hyx`, `line_a__dict`), and the two ABIs
collide.**

### Why the carrier-bridge attempts were the wrong direction

Earlier work tried to bridge the seam by making the nested dict-clone *read* the
by-value `g` as a carrier and *box* its result back (`emit_expr.c` ER2 fat-param
path + an unbox at the `line_a_x__mono` return). That converts the compile error
into the SEGV above and never becomes runtime-consistent, because the int64 the
dict-clone returns is not the boxed-aggregate pointer the unbox assumes. Those
band-aids were reverted; only the genuinely-correct decl/def reconciliation in
`emit_abi_forward_decl` (the `dict_clone_class` -> int64 override matching
`emit_fns.c`) was kept, and it is inert on green tests.

**The fix is to propagate the by-value specialization *into* the nested lens
calls so the whole chain stays by value and never boxes** -- not to bridge
between two ABIs at the seam.

---

## Design: by-value propagation into nested lens calls

Inside a by-value mono body for a composed lens (`line_a_x__mono`, minted only
once this plan lands), two nested applications must be redirected to by-value
targets:

1. **The direct concrete-lens application `(point-x g p)`** inside the adapter
   `(fn [p] (point-x g p))`. Today the existing VBM3 redirect only fires for a
   lens **param** application `(l g s)`, not for a **global** lens application
   `(point-x g p)`. Add a redirect for a direct concrete-lens application when it
   occurs in an `is_vl_wide_mono` context: rewrite `(point-x g p)` to the
   existing by-value `point_x__mono(g, p)` (which is already emitted for the
   simple lens). The adapter's `g` is the by-value twin, and `point_x__mono`
   consumes exactly that, so no box is created.

2. **The nested lens `(line-a adapter s)`** itself. `line-a` is a simple lens, so
   a by-value `line_a__mono` can be generated the same way `point_x__mono` is.
   The composed body `line_a_x__mono` then calls `line_a__mono(adapter, s)` where
   `adapter` is a by-value closure returning `(f Point)` by value. The result
   `(f Line)` is returned by value, matching `line_a_x__mono`'s declared
   `(Identity Line)` -- the decl/def already agree via the
   `emit_abi_forward_decl` reconciliation.

With both redirects, the box/unbox problem dissolves: there is no carrier
crossing anywhere in the composed chain.

### Registration changes

- **Re-admit composed lenses** to `resolve_walk`: instead of poisoning the
  consumer, register the composed lens as a concrete key AND recursively register
  its nested simple lenses (`point-x`, `line-a`) as concrete keys so their
  `<lens>__mono` bodies exist. This is a new *transitive lens-body* registration
  distinct from CM1's transitive *consumer-forwarding* registration: it walks the
  composed lens's body for nested lens applications and seeds a concrete key per
  nested lens under the same functor pin.
- Mint a by-value mono body for the composed lens (`line_a_x__mono`) via the same
  VBM2b machinery, with `is_vl_wide_mono` set so the two nested redirects fire in
  its body.
- Retire `has_composed_lens` poisoning once the above lands (or keep it as a
  backstop for a residual sub-shape a first cut does not cover -- e.g. a composed
  lens whose nested lens is itself composed, i.e. depth > 2).

### Adapter closure ABI

The adapter `(fn [p : Point] : (f Point) (point-x g p))` is built by value inside
`line_a_x__mono`. Its result type `(f Point)` must be spelled by value (the same
`f := Identity` substitution VBM2b already applies), and its captured `g` must be
the by-value twin (the existing `thunk_sym_override` / `inner_closure_spec_idx`
machinery that CM2 uses to give a per-spec closure its distinct by-value thunk).
The adapter is passed to `line_a__mono`, whose `g`-slot type is
`(-> Point (Identity Point))` by value -- so the adapter and `line_a__mono` agree
with no conversion.

---

## Work breakdown

- **CB1 -- nested-lens concrete registration.** In `mono_specs.c`, when a
  consumer resolves to a composed lens, register the composed lens's concrete key
  and recursively register each nested lens application's concrete key (same
  functor/focus/whole pin). Add a `lens_nested_lenses(const FnDef*)` walk that
  yields the global lenses applied in the body. Drop (or narrow) the
  `has_composed_lens` poison accordingly.
- **CB2 -- direct concrete-lens redirect.** In `emit_expr.c`, extend the
  poly-call redirect so a **direct** application `(point-x g p)` in an
  `is_vl_wide_mono` body rewrites to `point_x__mono(g, p)`. Key the target off
  the concrete lens name + the active spec's functor pin.
- **CB3 -- composed mono body emit.** Ensure VBM2b mints `line_a_x__mono` with
  `is_vl_wide_mono` and the correct `(f Line)` by-value result, and that the
  adapter closure inside it is emitted by value (result `(f Point)`, captured `g`
  the by-value twin).
- **CB4 -- fixture flip.** `van-laarhoven-lens-wide-compose` currently passes via
  the Path A fallback. Add an assertion (or a sibling `-mono` fixture) that the
  composed chain now emits **no** carrier box -- e.g. grep the emitted C for the
  absence of the boxing helper on the composed path, mirroring how
  `van-laarhoven-lens-wide-mono` checks the simple redirect.
- **CB5 -- depth > 2 / mixed nesting.** Decide coverage for a composed lens whose
  nested lens is itself composed. Either handle recursively (CB1's walk is
  already recursive) or keep the `has_composed_lens` backstop for that residual
  and document it.

---

## Test strategy

- `van-laarhoven-lens-wide-compose` must keep printing `7 / 700 / 2 / 0 / 42`
  (runtime parity is the invariant; the path underneath changes).
- Add a codegen check that the composed path no longer boxes (CB4).
- Full suite (`bash tests/run.sh`, 10-minute timeout) stays at its current count
  with 0 failures.
- Re-run every `van-laarhoven-lens-wide-*` fixture through the by-value path
  (the ground-truth sweep CM4 used) and confirm compose now compiles by value.

---

## Risks

- **Emit-path intricacy.** The redirects live in the same dense poly-call /
  fat-param region of `emit_expr.c` as the CM2/CM3 rewrites; a wrong turn
  regresses the simple path. Mitigate by gating every new branch on
  `current_abi_specialization->is_vl_wide_mono` plus the concrete-lens match, so
  non-composed code is provably untouched.
- **Adapter capture correctness.** The by-value `g` twin must reach the nested
  `point_x__mono` unchanged; getting the `thunk_sym_override` wiring wrong
  reintroduces the carrier read. The CM2 twin machinery is the proven template.
- **Deep nesting.** Depth > 2 composition may need the recursive walk to
  terminate on cycles (a lens that references itself); bound it and keep the
  Path A backstop for anything the walk declines.

---

## Related

- [../van-laarhoven-monomorphization-plan.md](../van-laarhoven-monomorphization-plan.md)
  -- Path B (VBM1-VBM4), the by-value mono body + VBM3 redirect
- [../van-laarhoven-consumer-mono-plan.md](../van-laarhoven-consumer-mono-plan.md)
  -- consumer monomorphization (CM1-CM4) + the CM4 composed-lens gate this plan
  removes
- [../../guides/lens-guide.md](../../guides/lens-guide.md) -- the user-facing
  functor-width section
