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

1. **Generic consumers** (`wide-generic`, `wide-compose`). `view`/`over`/`set`
   are `[S A]`-polymorphic, so their specs key `focus=tyvar whole=tyvar`
   (`--dump-mono-specs`). The mono body / consumer clone emit assumes a concrete
   focus/whole; with tyvars it produces conflicting `..._dict_..._spec_...`
   signatures and `incompatible type for argument 1 of 'run_hyid'`.
2. **Lens composition** (`wide-compose`). `line-a-x` composes `line-a` and
   `point-x` via an adapter `(line-a (fn [p] (point-x g p)) s)`. The nested lens
   dispatch inside the by-value mono body is not threaded correctly
   (`conflicting types for 'line_a__dict_..._spec_...'`).
3. **Receiver-reading `fmap` instances** (`wide-capture`, concrete focus/whole).
   Its `Functor Identity` preserves the tag -- `(fmap [i g] (make-struct Identity
   :wrapped (g (run-id i)) :tag (id-tag i)))` -- reading the receiver's word 1.
   The by-value `fmap` twin's receiver retyping (emit_module.c VBM2b, the
   `nsp->arg_types[0] = recv_ty` + method-param recovery block) mistypes the
   second read: `incompatible type for argument 1 of '__inst_Functor_fmap_Identity'`.
   The passing fixtures all use the tag-dropping `(fmap [i g] (mk-id (g (run-id
   i))))`, so this read pattern was never exercised on Path B.

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
