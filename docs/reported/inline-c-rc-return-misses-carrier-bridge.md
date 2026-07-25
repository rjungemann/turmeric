# An inline-C body returning `rc<T>` produces `-Wint-conversion` at its call sites

**Severity:** low (cosmetic in the generated C; correct on any 64-bit target)
**Status:** open
**Found by:** collections-cannot-hold-rc-values item 3, while writing
`stdlib/rcchain.tur`

## Summary

A `defn` whose declared return type is `rc<T>` but whose body is inline-C emits
a base function returning the **int64 carrier**. A call that feeds that result
into an `rc<T>` parameter is emitted with no bridge cast, so the C compiler
warns:

```turmeric
(defstruct S :move [tag : int])
(defstruct Chain :move [item : rc<S> next : rc<Chain>])

(defn chain-nil [] : rc<Chain>
  ```c
  return 0;
  ```)

(defn chain-cons [item : rc<S> rest : rc<Chain>] : rc<Chain>
  (rc/of (make-struct Chain item rest)))

(defn main [] : int
  (let [a (rc/of (make-struct S 1))
        c (chain-cons (rc/clone a) (chain-nil))]
    (println (rc/strong-count a)))
  0)
```

```
warning: passing argument 2 of 'chain_hycons' makes pointer from integer
         without a cast [-Wint-conversion]
   __auto_type __ps_162 = (chain_hycons(a_1289, __ps_161));
note: expected 'RcControlBlock *' but argument is of type 'int64_t'
```

The program is correct -- it runs and prints `2` -- and the two types have the
same representation on every target Turmeric supports. The defect is that a
warning-free build is not achievable from ordinary source: any user who writes
this shape gets warnings in their own build with nothing to fix.

## Root cause

Not fully traced. The pure-Turmeric body (`chain-cons`) is specialized and its
parameters lower to `RcControlBlock *`; the inline-C body (`chain-nil`) is not
return-specialized and lowers to `int64_t`. Nothing bridges the two at the call
site. The same shape with `__TUR_RET__` in the inline-C body warns in the other
direction (`returning 'RcControlBlock *' from a function with return type
'int64_t'`), which says the macro and the emitted base signature disagree about
which lowering applies.

This is adjacent to, but not the same as, the documented inline-C `option`/
`result` support ([docs/guides/inline-c-results-guide.md](../guides/inline-c-results-guide.md)):
those have typed builders (`tur_ok_ptr`, `tur_some_ptr`) that construct the
canonical layout. `rc<T>` has no equivalent -- the carrier *is* the control-block
pointer, so no builder is needed and none exists, and the gap is purely in how
the call site is typed.

## Workaround

Return the null rc as an ascription instead of inline-C. It lowers identically
and is warning-clean:

```turmeric
(defn chain-nil [] : rc<Chain>
  (:: 0 rc<Chain>))
```

`stdlib/rcchain.tur` does exactly this, and says why at the definition.

## Fix directions

1. Emit the carrier bridge at the call site when an argument's lowering
   (carrier) differs from the parameter's (`RcControlBlock *`) -- the same
   `emit_carrier_bridge` treatment other carrier boundaries already get.
2. Or return-specialize an inline-C body whose declared return type is
   concrete, so the base signature matches the declaration.

## Related

Worth noting while in this area: `(:: 0 rc<Chain>)` succeeds, but the
carrier-crossing rules added for
[collections-cannot-hold-rc-values](collections-cannot-hold-rc-values.md) item 2
reject an `int`/`rc` reinterpret at *call* sites. So `::` is not routed through
`call_wrap_reinterpret` and is not subject to the owning-value check. That is
what makes the workaround above possible, and it is also a hole in that check
worth closing deliberately rather than by accident -- an ascription can put an
unowned control-block pointer somewhere the check was written to prevent.
