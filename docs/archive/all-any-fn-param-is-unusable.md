# An all-`any` function type as a typed parameter is unusable compiled

**RESOLVED 2026-09-11.** Two defects, as the report said, and neither was
H7's:

- **Repro 1 (the callee's body).** Not the parameter's representation -- it IS
  the fat `int64_t` handle, and a `(fn [int] any)` twin dispatched through the
  thunk protocol fine.  The difference was the ARGUMENT: `(:: 4.5 any)` is an
  `EX_UNION_INJECT`, which `safe_to_delegate` does not admit, so the call was
  not delegated to the direct emitter and fell to the CPS IR's generic
  fallthrough, which builds a named `CT_LETCALL` / `CT_TAILCALL` and spells
  the callee by name -- `f(__t0)` against an `int64_t f`.  `src/passes/cps_ir.c`
  now routes a callee BINDING with no `FnDef` behind it (a fn-typed param or
  local) exactly like the binding-less indirect call: delegate with atomic
  args, otherwise evict.  A fn value never takes the named arm.
- **Repro 2 (the seam).** Direction 2 was right about the id and wrong about
  arity: the box carries the BOXED all-`any` id (H8's outbound adaptor makes
  every boxed fn a fat all-`any` closure, keyed `closure(fn ...)`), and the
  seam's plain unbox checked the BARE `(fn [] any)` id.  When
  `saffron_seam_fn_adaptor` declines an all-`any` target, `elab_call.c` now
  unboxes to the boxed twin of that type, so the cast keys on the id the widen
  stamped and the value flows on as the fat handle it is (no fatshim wrap).

Pinned by `tests/fixtures/all-any-fn-param` (both repros, plus an all-`any`
HOF applied twice, on both back ends).  The fuzzer's `all-any-fn-param` KNOWN
row is retired; `route_seam_fn` is back in the default pool.

**Severity (at filing): medium** -- a declared parameter type that cannot be called, on a
signature the Saffron dialect produces by default. Both a cc error and a
runtime panic, depending on how the value arrives. The interpreter handles
every shape below.

Found 2026-09-10 while fixing `saffron-dynamic-surface-pass` H7. It had been
masked: the fuzzer's `route_seam_fn` was suppressed by H7's `KNOWN` row, and
retiring that row is what surfaced it.

## Repro 1 -- the callee does not compile

```turmeric
#lang saffron
(defn tany [f : (fn [any] any)] : any (f (:: 4.5 any)))
(defn id [x] x)
(defn main [] : int (println (cast (tany (id (fn [v] v))) float)) 0)
```

```
error: called object 'f' is not a function or function pointer
  __t1 = f(__t0); /* cps->direct */
note: declared here
  static int64_t tany__cps(int64_t f, DK *__kont)
```

The parameter is emitted as a bare `int64_t` and then CALLED. This is the
callee's own body, so no caller can avoid it -- the function is uncompilable as
written. Interpreted: `4.5`.

## Repro 2 -- the seam into it panics

```turmeric
#lang saffron
(defn thru [x] x)
(defn call0 [f] (f))
(defn sf [g : (fn [] any)] : (fn [] any) g)
(defn main [] : int
  (println (call0 (thru (sf (thru (fn [] 288))))))
  0)
```

```
panic: cast: any holds a function this cast cannot accept -- a different
       signature, or a closure that captures where a plain function is required
```

Interpreted: `288`.

## Why it is not H7, and not fixed by H7's fix

H7 is the seam from an `any` into a typed fn parameter, and it is fixed: the
seam synthesises a marshalling adaptor
(`saffron_seam_fn_adaptor`, elab_call.c). That adaptor deliberately DECLINES an
all-`any` target, and correctly so -- an all-`any` signature is exactly the
representation the box already holds (H8's outbound adaptor makes every boxed
function all-`any`), so there is nothing to marshal and a wrapper would only add
a call.

Lifting that decline was tried and does not fix either repro, which is the
evidence that this is a separate defect rather than a gap in H7's fix:

- Repro 1 fails inside the CALLEE's body, before any caller is involved.
- Repro 2 still panics at the cast with the decline lifted.

## Fix directions

1. **The parameter's representation (repro 1).** A `(fn [any] any)` parameter
   is emitted as `int64_t` where a `(fn [int] int)` one is emitted as the
   three-word `tur_poly_fn_t`. Find why the all-`any` signature takes the
   carrier rather than the poly-fn representation, and give it the same one --
   then `f(__t0)` becomes a real call. This is likely the whole of repro 1.

2. **The box id at the seam (repro 2).** The value arrives with the all-`any`
   fn id and the cast wants the id of the declared `(fn [] any)`; those should
   be the SAME id, and the fact that they are not suggests the arity-0 case
   mints a distinct id. Compare `emit_any_type_id` for `(fn [] any)` against
   the id H8's outbound adaptor stamps.

Do NOT "fix" this by relaxing `__tur_any_cast_check` for fn ids. That was
measured under H7 and is a silent wrong answer: a callee then calls an
all-`any` shim with raw machine words where 16-byte tagged values are expected.

## Test-suite state

`tests/saffron-fuzz-src.py` carries a `KNOWN` row `("all-any-fn-param",
("route_seam_fn",))` so the generator does not emit the shape. Retire that row
when this is fixed; the route itself is correct now (its composition bug --
the route consumed the thunk its leg's unwrap still had to call -- was fixed in
the same change).

## Guides to update when fixed

- `docs/guides/saffron-guide.md` -- the function-value section, which does not
  say that an all-`any` fn parameter is currently unusable.
