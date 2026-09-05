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

## Resolved 2026-09-05

`find_mono_clone_for_call` (emit_cps_ir.c) matched a specialization on argument
C types alone. Two new helpers beside it make the call's own RESULT type a
discriminator:

- `cps_call_result_discriminator` -- the call's resolved result type, or NULL
  when it decides nothing. The NULL case is the load-bearing half: a call whose
  result is still a type variable, or an app with an abstract head, c-names to
  the int64 carrier exactly as a genuine `int` does, so filtering on it would
  reject the correct concrete clone for the `^Show a` wrappers RC1/RC2 resolve
  through this same function. Deciding nothing leaves the arg-only behaviour
  untouched.
- `spec_result_matches` -- accepts every spec when the discriminator is NULL, so
  the change is a pure NARROWING. It can only turn a wrong hit into NULL (which
  falls back to the erased base, the pre-existing conservative path) or turn an
  ambiguity into a unique correct hit. It cannot introduce a match that did not
  exist.

Applied to both matching passes (the primary one that skips scalar args, and the
RC2/2b.3 stricter tie-break), and threaded through all three call sites --
`CT_LETCALL` plus both `CT_TAILCALL` clone lookups -- from the `call_expr` the
CPS IR already retains on each node for `emit_reresolve_method_call`.

Both faces fixed by the one change, which is what the narrowing predicts:

- **Repro 1** picked the record spec because it was the ONLY spec registered
  (`A = int` needs none -- the int64 result IS the carrier, so the base is
  correct). The result filter rejects it, the lookup returns NULL, and the call
  falls to `bt_hyscope`, which is right.
- **Repro 2** had two specs, both matching on args, so the lookup returned NULL
  and `cps->direct` called the erased base. The result filter leaves exactly one
  match, so the QPair site now resolves to its own clone.

**Pinned by two fixtures, one per face**, because the faces mask each other in a
combined file -- a pre-fix run sees only the build error:

- `tests/fixtures/cps-spec-selected-by-result-type` -- the silent wrong answer.
  Compiles pre-fix and prints an address. Values are READ, not merely built.
- `tests/fixtures/cps-spec-erased-base-fallback-typechecks` -- the build
  failure. No trail, no `bt-scope`, no flags: a plain `[A]` defn.

Both verified against a pre-fix binary (`emit_cps_ir.c` copied aside and
restored, not stashed): garbage-then-28 for the first, `cc invocation failed`
for the second.

**The sibling report is NOT fixed by this**, and the prediction in its "fix
directions" that it would ride along was wrong -- checked rather than assumed.
[region-bracket-lost-when-bt-scope-specializes](../reported/region-bracket-lost-when-bt-scope-specializes.md)
still reproduces: the specialization now resolves correctly, and the
`/* cps->cps */` arm it takes still emits no `tur_region_push()`. Same function,
genuinely different gap.

**Validation.** Suite 2799/0 (2797 + the two new fixtures; no snapshot moved,
which is itself the evidence that no existing resolution changed), turi
1911/0/856, regions seam 12/0, leak-check 83/0, sr2 seam 57/0, cc-warn ratchet
OK.
