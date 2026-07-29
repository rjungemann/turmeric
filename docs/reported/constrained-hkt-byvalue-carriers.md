---
status: open
severity: medium
discovered: 2026-07-29
area: compiler (constrained HKT dispatch / poly carrier)
---

# Constrained HKT polymorphism: by-value carriers, remaining seams

## Summary

A constrained kind-polymorphic function (`[^m] [^Monad m x : (m int)]`) is
compiled once and dispatches through a dictionary the caller resolves. The
carrier ABI for **by-value** type constructors (stdlib `Option`, `Result` --
real structs, not int-carrier `defopaque`s) is now correct for the
dictionary-passed path; two narrower seams remain.

> **Fixed 2026-07-29:** the continuation handed to a dict-dispatched method was
> a struct-returning thunk cast to an int64-returning function pointer -- an
> x86-64 return-ABI mismatch that segfaulted every by-value instantiation. The
> poly-wrap now requests the carrier-spill shim when the receiver is the
> abstract constructor. See
> [../archive/history/constrained-hkt-byvalue-carrier-abi.md](../archive/history/constrained-hkt-byvalue-carrier-abi.md).
> Fixtures: `hkt-constrained-byvalue-carrier`,
> `hkt-constrained-byvalue-bind-pure`.

## Seam 1 -- monomorphized spec return is not unboxed

A *wider* by-value container fails to compile: the spec declares the aggregate
return but its body yields the int64 carrier.

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

## Seam 2 -- `Result` cannot fill a unary `(m int)`

    (defn poly-bind [^m] [^Monad m x : (m int)] : (m int) ...)
    (poly-bind (ok-int 4))
    error [TUR-E0001]: expected (type-app tyvar 'm' int),
                       got (type-app (type-app Result int) cstr)

`Result` is binary, so filling `m` needs the partially-applied head
`(Result _ cstr)` -- the same shape `definstance Monad [(Result _ B)]` already
uses. Type-constructor *application* accepts it; unification against a unary
`(m int)` does not. Until that lands, only unary constructors can instantiate a
constrained poly fn.

## Related

- [../archive/constrained-hkt-lifted-lambda-keeps-representative-instance.md](../archive/constrained-hkt-lifted-lambda-keeps-representative-instance.md)
  -- the representative-instance dispatch bugs (spec-level and lifted-lambda),
  both RESOLVED 2026-07-29; the lambda case via Route B dictionary passing.
- [result-monad-bind-typed-boundary-miscompiles.md](result-monad-bind-typed-boundary-miscompiles.md)
  -- the same carrier-vs-by-value confusion on the plain typed-boundary path.
