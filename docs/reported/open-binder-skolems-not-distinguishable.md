---
title: Two nested `open` binders produce skolems that are not distinguishable by `type_eq`
category: Elaborator gap / existential skolemization
severity: Latent expressiveness hole. Two nested `(open ... [n_i x_i] ...)` introduce two abstract size binders `n_1` and `n_2` that the elaborator represents identically as `TY_STRUCT` with `def=NULL`. The call-site unifier checks bound-tyvar consistency via `type_eq`, which therefore treats `n_1` and `n_2` as the same type. A cross-skolem call like `(sized-buf-copy! a b)` where `a` and `b` came from separate opens type-checks instead of rejecting under TUR-E0260.
description: After the pack/open phantom-opaque fix landed (parent report), nested opens produce `(SizedBuf <skolem_a>)` and `(SizedBuf <skolem_b>)`. Calling `sized-buf-copy! [n] [dst : (SizedBuf n) src : (SizedBuf n)]` should bind `n=skolem_a` from the first arg and then reject `skolem_b` for the second arg as TUR-E0260. Instead the call accepts because both skolems are the same anonymous `TY_STRUCT def=NULL` value, and `type_eq` returns true.
status: OPEN. Discovered while writing the cross-open reject fixture for the parent report. Closing this would give existential-lifted SizedBufs the same compile-time cross-parameter rejection that concrete `(Static k)` literals already enjoy under the SZ8 unifier.
---

# Two nested `open` binders are indistinguishable

## Minimal repro

```turmeric
(load "stdlib/sized.tur")
(load "stdlib/sized-buf.tur")

(defn mk2 [] : (SizedBuf int) (:: (sized-buf-new-zeroed 2) :SizedBuf))
(defn mk3 [] : (SizedBuf int) (:: (sized-buf-new-zeroed 3) :SizedBuf))

(defn main [] : int
  (let [pa (pack (mk2) (exists [n] (SizedBuf n)))
        pb (pack (mk3) (exists [n] (SizedBuf n)))]
    (open pa [na a]
      (open pb [nb b]
        (do
          (sized-buf-copy! a b)   ; should be TUR-E0260; today type-checks
          (println 0)))))
  0)
```

Type-checks (no diagnostic). Codegen then trips
`docs/reported/open-monomorphizes-polymorphic-fn-only-partially.md`
because `sized-buf-copy!` is reached only through the open body, but
that is the *separate* gap; the load-bearing point of this report is
that the unifier failed to reject the cross-skolem mismatch in the
first place.

## Observed vs expected

- Observed: call elaborates cleanly; `na` and `nb` are treated as the
  same type because both are anonymous `TY_STRUCT def=NULL` and
  `type_eq` cannot tell them apart.
- Expected: TUR-E0260 "size mismatch across parameters" -- the same
  diagnostic that fires for concrete `(SizedBuf (Static 2))` vs
  `(SizedBuf (Static 3))` in `errors/sized-buf-cross-param-reject`.

## Root-cause hypothesis

`call_collect_type_bindings` (`src/compiler/elab_call.c:129`) walks
arg-by-arg, binding each named tyvar's first occurrence to the actual
type and checking later occurrences via `type_eq`. When the actual
type is `TY_STRUCT def=NULL` (a tyvar / skolem), every fresh skolem
compares equal to every other, so the late-occurrence check passes.

`elab_open` (`src/compiler/elab_types.c`) does not currently mint a
*distinct* tyvar identity for each open's abstract binder; the v_type
just inherits the body type, in which the bound tyvar from
`(exists [n] ...)` is the same anonymous form across every open in
the program.

## Proposed fix directions

1. **Mint a fresh named tyvar per open.** At open time, allocate a
   unique tyvar name (e.g. `__open_skolem_<id>`) and substitute it
   for the existential's bound binder in the body type. The unifier
   compares names, so distinct opens then collide on the late-occurrence
   check.
2. **Track skolem identity through a side table.** Less invasive than
   (1) but more state to maintain; (1) is the cleaner shape.

(1) is the principled fix; the open skolem counter already exists
(`e->open_skolem_next`).

## Validation

- Add `tests/fixtures/errors/sized-buf-existential-cross-open-reject/`
  using the repro above; `expected.diag` matches TUR-E0260.
- Existing `tests/fixtures/sized-buf-existential-pack-open` (single
  open, single call) stays green.

## Related

- `docs/reported/pack-open-phantom-opaque-body-type-collapses.md` --
  parent report; this gap surfaced while writing the parent's reject
  fixture.
- `docs/reported/open-monomorphizes-polymorphic-fn-only-partially.md`
  -- sibling codegen gap. Closing either does not require closing the
  other.
