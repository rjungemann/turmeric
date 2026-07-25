# An inline-C body returning `rc<T>` produces `-Wint-conversion` at its call sites

**Severity:** low (cosmetic in the generated C; correct on any 64-bit target)
**Status:** RESOLVED 2026-07-25
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

## Resolution

Neither of the two directions first considered. The declaration was right all
along and the emitter simply ignored it: `emit_fns.c` already carries a list of
escape hatches -- `typed_ptr`, `typed_struct`, `typed_cfnptr`, `typed_heap_spec`,
`typed_byval_adt` -- for declared return types that lower concretely *even from
an inline-C body*. An owning return belongs in that list for exactly the reason
`typed_ptr` does: it **is** a pointer, and the carrier holds precisely its bits.
It was missing.

Adding `typed_rc` (rc/weak/ref/lref) there, and to its mirror in the
forward-declaration emitter in `emit_module.c` -- the two must agree or the
prototype conflicts with the definition -- makes the base return
`RcControlBlock *`, so no call site needs a bridge at all. `return 0;` in the
body stays valid as a null pointer constant, which is the shape that matters:
a null rc is essentially the only reason to reach for inline-C at an rc return.

Pinned by `tests/fixtures/inline-c-rc-return-typed`, which asserts refcounts
rather than just compilation -- the interesting failure mode of a wrong return
lowering is a handle that survives the type checker and then miscounts. The
suite passed unchanged with no snapshot churn: no inline-C body in tree declared
an rc return, which is itself why this went unnoticed.

## Related

The `::` hole this workaround relied on has since been closed deliberately
rather than left as an accident (see
[collections-cannot-hold-rc-values](collections-cannot-hold-rc-values.md)
item 2, ascription side). `::` now rejects **owning -> anything** outright, and
allows **anything -> owning** only for the literal `0`. The workaround above is
exactly that literal-0 form, so it survives on purpose rather than by oversight.
