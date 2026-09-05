# A CPS-lowered call picks the wrong ABI specialization (or none)

**Severity: high -- silent wrong answer on default flags, and a hard build
failure on a neighbouring shape.** Filed 2026-09-05, found while categorizing
RM2's spine residue (`carrier-sum-option-boxes-have-no-owner`).

A polymorphic function instantiated at two result types whose representations
differ, called from a CPS-lowered caller, is emitted against the WRONG
specialization. No flag is needed; `--enable=regions` is not involved.

## Repro 1 -- silent wrong answer, through stdlib

```turmeric
(load "stdlib/trail.tur")
(defdata PPair (PIP :int :int))

(defn s-int  [n : int] : int   (bt-scope (fn [] (+ n 20))))
(defn s-rec  [n : int] : PPair (bt-scope (fn [] (PIP n 20))))

(defn main [] : int
  (println (s-int 8))                                ;; want 28
  (match (s-rec 8) (PIP a b) (println (+ a b)))      ;; want 28
  0)
```

```
$ ./build/tur run repro.tur
94547359593728        <-- a pointer, printed as an int
28
```

Delete `s-rec` and `s-int` prints `28`. The two are order-independent: putting
`s-rec` first still breaks the `int` one.

`bt-scope` is `stdlib/trail.tur`'s bracket, so this is reachable through the
shipped API -- any program that brackets at two different result types.

## Repro 2 -- the same defect as a build failure

```turmeric
(defdata QPair (QIP :int :int))
(defn runit [A] [^fat body : (fn [] A)] : A (body))
(defn r-flt [n : int] : float (runit (fn [] 7.1)))
(defn r-rec [n : int] : QPair (runit (fn [] (QIP n 20))))
(defn main [] : int
  (println (r-flt 8))
  (match (r-rec 8) (QIP a b) (println (+ a b)))
  0)
```

```
error: incompatible types when assigning to type 'tur_adt_QPair' from type 'int64_t'
tur: cc invocation failed (status 256)
```

Nothing about `bt-scope` is required -- a plain `[A]` defn reproduces it, with
or without `^fat`.

## Root cause

Both specializations ARE emitted. The bug is at the CPS emitter's call arms,
which do not select among them by the call's own result type. Two faces:

- **`/* cps->cps */`** picks *a* spec and it can be the wrong one. In repro 1
  BOTH call sites emit
  `bt_scope__spec__tur_adt_PPair_int64_t__cps(...)`; no
  `bt_scope__spec__int64_t_int64_t` is emitted at all. The `int` caller then
  reads a pointer-to-box as its `int` result -- the printed garbage.
- **`/* cps->direct */`** skips specialization entirely and calls the ERASED
  base. In repro 2 `r_hyrec__cps` emits
  `int64_t __t186 = runit(...); /* cps->direct */` -- the `int64_t` carrier --
  and then boxes `__t186` as though it were a `tur_adt_QPair` value, which is
  the C error above. The sibling `r-flt`, emitted on the DIRECT path, correctly
  calls `runit__spec__double_int64_t`.

So the specialization registry is right and the direct path consults it; the
CPS path does not.

## Why the suite is green

Every `bt-scope` call site in the tree returns `int`, `bool` or `void`
(`region-scope-*`, `sx2-trail-combinators`, `cps-bt-scope-thunk-calls-user-fn`,
`stdlib/backtrack-dfs.tur`'s `dfs-solve`). All of those are carrier-transparent,
so the erased base and every specialization agree and the mis-selection is
invisible. **No fixture instantiates a CPS-reached polymorphic function at a
by-value aggregate.** A fixture that does is the first thing this needs.

## Fix directions

1. Make the CPS call arms ask the same question the direct path asks -- resolve
   the callee against the call's own result type before emitting the name. This
   is where the fix belongs; the two arms are in the CPS emitter's call
   lowering, beside the `/* cps->cps */` and `/* cps->direct */` comments they
   print.
2. Failing that, a tripwire: emitting a `cps->direct` call to an erased base
   whose result type does not c-name to `int64_t` at the call site is always
   wrong, and is cheap to assert.

Direction 1 subsumes
[region-bracket-lost-when-bt-scope-specializes](region-bracket-lost-when-bt-scope-specializes.md),
which is the same arm losing a different thing.

## Workaround

Instantiate a CPS-reached polymorphic bracket at ONE result representation per
program, or hand the aggregate out through a scalar (return an index, or write
into a cell) rather than as the bracket's value.
