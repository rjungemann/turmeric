---
status: RESOLVED (2026-07-29) -- carrier ABI + seam 1 fixed; seam 2 extracted to its own report
severity: medium
discovered: 2026-07-29
area: compiler (constrained HKT dispatch / poly carrier)
---

# Constrained HKT polymorphism: by-value carriers

## Summary

A constrained kind-polymorphic function (`[^m] [^Monad m x : (m int)]`) is
compiled once and dispatches through a dictionary the caller resolves. The
carrier ABI for **by-value** type constructors (real structs, not int-carrier
`defopaque`s) is correct end to end: the return-ABI mismatch was fixed by the
carrier-spill shim (below), and seam 1 -- the wider-container spec-return
failure -- was resolved by Route B, which routes direct calls through the dict
clone and off the broken monomorphized-spec return path entirely (fixture:
`hkt-constrained-wide-byvalue-carrier`).

The one remaining gap (formerly seam 2) is an expressiveness limit in call-site
unification, not a carrier fault, and now has its own report:
[constrained-hkt-abstract-var-requires-last-param-free.md](constrained-hkt-abstract-var-requires-last-param-free.md)
(since RESOLVED).  Note that report re-diagnoses it: the "binary head" framing
used below is wrong -- arity is not the issue (`Either` abstracts fine); the
free slot had to be the constructor's LAST, which the hole-in-Type fix lifted.

> **Fixed 2026-07-29:** the continuation handed to a dict-dispatched method was
> a struct-returning thunk cast to an int64-returning function pointer -- an
> x86-64 return-ABI mismatch that segfaulted every by-value instantiation. The
> poly-wrap now requests the carrier-spill shim when the receiver is the
> abstract constructor. See
> [../archive/history/constrained-hkt-byvalue-carrier-abi.md](constrained-hkt-byvalue-carrier-abi.md).
> Fixtures: `hkt-constrained-byvalue-carrier`,
> `hkt-constrained-byvalue-bind-pure`.

## Seam 1 -- monomorphized spec return is not unboxed (RESOLVED by Route B)

A *wider* by-value container failed to compile: the spec declared the aggregate
return but its body yielded the int64 carrier.

    $ cat > /tmp/p.tur <<'EOF'
    (defstruct Pad2 [A] (a :int) (b :int) (val A))
    (defn mk-pad [A] [x : A] : (Pad2 A) (make-struct Pad2 :a 11 :b 22 :val x))
    (defn pad-val [A] [p : (Pad2 A)] : A (.val p))
    (definstance Monad [Pad2] (bind [ma k] (k (.val ma))))
    (defn poly-bind [^m] [^Monad m x : (m int)] : (m int)
      (bind x (fn [v] (mk-pad (* v 2)))))
    (defn main [] : int (println (pad-val (poly-bind (mk-pad 7)))) 0)
    EOF
    $ ./build/tur run /tmp/p.tur
    error: incompatible types when returning type 'int64_t' but 'tur_adt_Pad2__int'
           was expected  (in poly_bind__spec__tur_adt_Pad2__int_tur_adt_Pad2__int)

Expected: `14`. `Option` escapes this because its spec return lands on a
different branch of the return-type selection in `emit_fns.c` (the `use_abi_spec`
ladder around the `inst_method_carrier_spill` flag); the non-instance
constrained-poly spec has no matching carrier-spill return path.

**Fix direction.** Add the missing case to that ladder: a non-`__inst_` spec
whose declared result is a by-value aggregate but whose tail is a
carrier-returning method call needs `emit_agg_unbox` at the return. That ladder
has ~6 tuned branches referencing several plans, so it wants its own focused
pass rather than a tail-end addition.

## Seam 2 -- `Result` cannot fill a unary `(m int)` (EXTRACTED)

Moved to its own report, re-diagnosed, and since RESOLVED:
[constrained-hkt-abstract-var-requires-last-param-free.md](constrained-hkt-abstract-var-requires-last-param-free.md).

## Related

- [../archive/constrained-hkt-lifted-lambda-keeps-representative-instance.md](constrained-hkt-lifted-lambda-keeps-representative-instance.md)
  -- the representative-instance dispatch bugs (spec-level and lifted-lambda),
  both RESOLVED 2026-07-29; the lambda case via Route B dictionary passing.
- [result-monad-bind-typed-boundary-miscompiles.md](result-monad-bind-typed-boundary-miscompiles.md)
  -- the same carrier-vs-by-value confusion on the plain typed-boundary path.
