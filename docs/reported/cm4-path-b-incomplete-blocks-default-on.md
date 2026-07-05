# CM4 default-on is blocked: Path B (consumer-mono) is incomplete

**Severity:** high for CM4 (it is the graduation gate). Not a bug in shipping
behavior today -- Path B is opt-in (`--enable=vl-wide-mono`) and every failing
shape works on the default Path A carrier. The problem is only exposed by making
Path B unconditional.

## One-line

The CM4 premise -- "every statically-resolvable wide lens use compiles to Path B"
-- is false. Flipping `vl-wide-mono` to default-on regresses three
`van-laarhoven-lens-wide-*` fixtures whose shapes Path A handles uniformly but
Path B's by-value mono-body emit (VBM2b) mis-specializes. These are latent Path B
bugs never caught, because those fixtures are flagless and only ever ran Path A.

## Repro

Remove the `g_opt_vl_wide_mono` guards (main.c:387, elab_call.c:6004,
emit_module.c VBM2b + CM2 blocks, emit_expr.c CM3 rewrite + VBM3 redirect) so
Path B fires unconditionally, then `bash tests/run.sh`:

```
FAIL van-laarhoven-lens-wide-capture  -- tur build failed
FAIL van-laarhoven-lens-wide-compose  -- tur build failed
FAIL van-laarhoven-lens-wide-generic  -- tur build failed
```

Each fails at the C-compile step (Path B emits ill-typed C), not at runtime.

## The three Path B gaps

1. **Generic consumers** (`wide-generic`, `wide-compose`). **FIXED (CM4).**
   `view`/`over`/`set` are `[S A]`-polymorphic, so their specs key
   `focus=tyvar whole=tyvar`. The by-value redirect was firing in the consumer's
   GENERIC CARRIER base body (no active ABI spec), baking `point_x__mono` into
   the polymorphic body and feeding the carrier `run_hyid` a by-value aggregate.
   Fix: `mono_spec_consumer_is_generic` + suppress the |set|==1 redirect when
   `current_abi_specialization == NULL && generic` (the concrete ABI-spec body,
   which has an active spec, still redirects). `wide-generic` now passes on
   Path B; `wide-compose` advances to gap 2 below.
2. **Lens composition** (`wide-compose`). OPEN. **ROOT CAUSE FOUND (ASan).** The
   composed lens `line-a-x` receives a BY-VALUE `g` from the wide consumer
   (`over__spec` builds `g.__fn = __fn_1335__spec` -- returns `Identity__int` BY
   VALUE), but its nested `point-x` lowered to the CARRIER `point_hyx_...`, which
   invokes `g` with a carrier cast `((int64_t (*)(void*, int64_t))g[0])(...)`. So
   it reads only the FIRST word of the by-value struct (`700 = h(a)`) as if it
   were a boxed pointer and feeds it to `fmap`, whose `run_hyid(0x2bc)` derefs
   `700` as a `tur_adt_Identity *` -> SEGV. In short: the composition mixes a
   BY-VALUE outer `g` with CARRIER-lowered nested lenses (`point_hyx`,
   `line_a__dict`), and the two ABIs collide.

   **The real fix is by-value propagation into the nested lens calls**, NOT the
   carrier bridges I tried: the nested `(point-x g p)` inside `line_a_x__mono`'s
   adapter must redirect to the by-value `point_x__mono` (which already exists!),
   and the nested `(line-a adapter s)` must resolve to a by-value `line_a__mono`
   (needs to be generated) -- so the whole composed chain stays by value and
   never boxes. This is a new redirect for a DIRECT concrete-lens application in
   an is_vl_wide_mono context (the existing VBM3 redirect only fires for a lens
   PARAM `(l g s)`, not a global lens `(point-x g p)`), plus generating a mono
   for the nested lens. Substantial but principled; it dissolves the box/unbox
   problem entirely rather than patching it.

   **Superseded band-aids (the carrier approach, now known wrong for this shape):**
   `emit_abi_forward_decl`'s `dict_clone_class` -> int64 reconciliation is a
   genuine decl/def fix and stays; the ER2 carrier `g`-call fix (6118) makes the
   dict-clone READ the by-value `g` as a carrier -- it turns the compile error
   into the SEGV above, so it is part of the wrong (carrier) direction. Both are
   inert on green tests (composition is flagless -> Path A). Do NOT build further
   on the carrier bridges; take the by-value-propagation path.

   (Historical, pre-root-cause notes:) now a RUNTIME issue, no longer a compile
   error. Fixed so far: (i) the
   decl/def reconciliation (above); (ii) the dict-clone's `g`-call now returns the
   int64 carrier (`emit_expr.c` ER2 fat-param path, gated on
   `dict_clone_class` + a concrete non-carrier aggregate result), matching the
   boxing adapter and the carrier `fmap` slot -- the 6118 arg-type error is gone.
   REMAINING: `line_a_x__mono` returns the dict-clone's int64 carrier but declares
   the by-value `(Identity Line)`. Adding a carrier->concrete unbox at the call
   result (`*(tur_adt_Identity__Line *)(intptr_t)(...)`) makes it COMPILE but
   SEGFAULT -- the int64 the dict-clone returns is not the boxed-aggregate pointer
   the unbox assumes, so the whole adapter -> dict-clone -> carrier-fmap -> unbox
   chain is not runtime-consistent. The real fix is to make the carrier box/unbox
   line up end-to-end (the carrier `fmap` must box the `(f S)` the same way the
   unbox reads it), or -- cleaner -- to propagate the by-value specialization INTO
   the nested lens so `line-a` becomes a by-value `line_a__mono` (no dict-clone,
   no boxing) instead of a carrier dict-clone. The latter is the structural fix
   and avoids the box/unbox entirely. Deepest of the three; deep MB2.5/WF3.

   (Historical detail:) `line-a-x`
   composes `line-a` and `point-x` via `(line-a (fn [p] (point-x g p)) s)`.
   Inside the by-value mono body `line_a_x__mono`, the nested `line-a` is lowered
   through a dict-clone `line_a__dict_..._spec_...` that MB2.5 forces to the int64
   carrier. **Advanced:** `emit_abi_forward_decl` now applies the same
   `dict_clone_class` -> int64 override as `emit_fns.c`, so the decl/def agree
   (the `conflicting types` error is gone). **Remaining:** the composition's
   internal ADAPTER closure `(fn [p] (point-x g p))` is built by value in
   `line_a_x__mono` (is_vl_wide_mono) but crosses into the CARRIER dict-clone,
   which dispatches `fmap` through the runtime dict (int64). Two ABI-boundary
   bridges are still missing: (a) box the adapter's by-value `(f Point)` result
   before the carrier `fmap` slot (`incompatible type for argument 1 of ...
   __dict_...`), and (b) unbox the dict-clone's int64 result back to
   `tur_adt_Identity__Line` at the `line_a_x__mono` return. Root: a composed
   wide lens straddles by-value (outer) and carrier (inner dict-clone) ABIs, and
   the adapter needs the carrier ABI on that crossing (the same g-ABI-per-context
   problem CM2's twin solved, now at the composition seam). Deep MB2.5/WF3.
3. **Receiver-reading `fmap` instances** (`wide-capture`, concrete focus/whole).
   OPEN. Its `Functor Identity` preserves the tag -- `(fmap [i g] (make-struct
   Identity :wrapped (g (run-id i)) :tag (id-tag i)))` -- reading the receiver `i`
   TWICE (`run-id i` and `id-tag i`). The by-value `fmap` twin
   (`__inst_Functor_fmap_Identity__spec__...`) is NOT minted for this shape, so
   `point_x__mono` calls the carrier `__inst_Functor_fmap_Identity` (int64) with a
   by-value `Identity` arg: `incompatible type for argument 1`. The twin-minting
   scan + receiver retyping (`emit_module.c` VBM2b, the `nsp->arg_types[0] =
   recv_ty` + method-param recovery block) does not cover a fmap whose body reads
   the receiver beyond the single `run-id`. The passing fixtures all use the
   tag-dropping `(fmap [i g] (mk-id (g (run-id i))))`, so this was never
   exercised. Deep VBM2b twin-minting.

## Options for CM4

- **(A) Complete Path B, then default-on.** Fix the three gaps in the VBM2b
  mono-body / clone emit (generic focus/whole, nested-composition dispatch,
  receiver-reading fmap twins). Substantial and in the intricate emit path;
  arguably its own slice/plan. Then default-on is a clean flip.
- **(B) Partial graduation with a conservative fallback gate (R2-aligned).**
  Default-on Path B ONLY where it is proven correct, fall back to Path A
  otherwise. `focus/whole == tyvar` is a clean, reliable skip (covers gaps 1 and
  the composition case, which only arises via the generic consumers here). Gap 3
  (concrete focus/whole + receiver-reading fmap) has no cheap structural signal,
  so it would need either a fix or a narrower functor-shape gate. Risk: a gate
  that is not provably complete can still let an unhandled shape through.
- **(C) Defer graduation; keep the flag.** Land the non-controversial CM4 parts
  (doc updates, archive the parent plan) but leave `vl-wide-mono` opt-in until
  Path B is complete. Honest, zero-risk, but does not retire the flag.

## Recommendation

The runtime-selected-lens residual that CM4's R1/R2 decision was about turns out
to be a non-issue -- it is already rejected by the general rank-2
"argument must be a named function" restriction (a let-bound lens hits a separate
pre-existing codegen bug; see below). So the ONLY thing standing between here and
graduation is Path B completeness, i.e. option (A). Until that lands, (C) is the
safe status quo and (B) is a middle ground that graduates the common
(monomorphic, simple-functor) case while explicitly keeping generic/compositional
lenses on Path A.

## Adjacent pre-existing bug (out of scope, noted)

A let-bound named lens -- `(let [m point-x] (set-px m ...))` -- fails to compile
today on BOTH paths (`'point_hyx' undeclared` in the generated C), independent of
consumer-mono. The rank-2 restriction accepts `m` (a local var bound to a named
lens), but codegen references an undeclared symbol. Worth its own report if it
matters; it does not affect the CM4 decision.
