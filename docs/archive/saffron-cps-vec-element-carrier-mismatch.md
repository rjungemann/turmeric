# A Saffron function that both performs and touches a vector emits uncompilable C

**RESOLVED 2026-09-14.** Two fixes, one at each end of a vector, because the
report's two repros were genuinely two defects sharing a cause -- the CPS arm
not performing the element seam the direct emitter performs:

- **PUSH (repro B).** `atoms_csv_call_typed` (emit_cps_ir.c) correctly says a
  by-value aggregate's concrete->carrier crossing is "a spill+address bridge,
  not this cast" and leaves it to the atom emission -- but nothing downstream
  performed that bridge for a `tur_tagged_t` into a carrier-shaped slot, so the
  16-byte struct was passed bare into `vec_hypush_ex`'s `int64_t val`. It now
  heap-boxes and passes the address, spelled exactly as the direct emitter
  spells it at the same site. The box is the element's, owned by the container,
  so it is deliberately not reaped.
- **READ (repro A).** The `CT_LETCALL` cps->direct arm assigned the raw carrier
  word into a `tur_tagged_t` binder. It now dereferences, keyed on both sides
  (binder is the aggregate AND the callee really returns the carrier, per the
  signature side table) -- the CPS twin of `bridge_control_result_int_ptr`'s
  `temp_is_tagged` arm in emit_expr.c, which is the same fix made for the direct
  path under saffron-lang-plan S6.

The report's "the element seam, on the CPS arm only" was right, and its
observation that `vec-len` and a user function over the same vector are fine
was the load-bearing one: it is what said the trigger is the element WORD
crossing and not vectors in general, which is why both fixes are keyed on the
`tur_tagged_t` C spelling rather than on the callee being a vector helper.

Pinned by `tests/fixtures/saffron-cps-vec-element-and-perform`, which carries
both directions plus a recursive walk that runs them in one CPS body, and keeps
`vec-len`/`vec-fold` alongside as the control. Both back ends agree.

The tour guide's advice to keep vectors out of a function that performs is
removed; its worked example no longer has to route around this.

---

**Severity: high -- build breaker.** In a `#lang saffron` file, a function
containing a `perform` (so it is CPS-lowered) plus a vector element read or a
`[...]` literal emits C that the host compiler rejects: the CPS arm passes the
16-byte `tur_tagged_t` where the vector helpers take an `int64_t` carrier word.
`cc invocation failed`, so nothing runs.

Found writing `docs/guides/introducing-saffron.md`; it is why that guide's
fixture keeps its `handle` forms in helper functions rather than in `main`.

## Repro A -- element read inside a performing function

```turmeric
#lang saffron
(defeffect Ask [] : int)
(defn f [v]
  (do (perform (Ask))
      (vec-get v 0)))
(defn main []
  (println (handle (f [7.1 2.5]) (Ask [] k) (resume k 41)))
  0)
```

```
In function 'f_pf0':
error: incompatible types when assigning to type 'tur_tagged_t' from type 'int64_t'
tur: cc invocation failed (status 256)
```

## Repro B -- a literal in a function that also handles

```turmeric
#lang saffron
(defeffect Ask [] : int)
(defn work [] (+ (perform (Ask)) 1))
(defn main []
  (println (vec-len [1 2 3]))
  (println (handle (work) (Ask [] k) (resume k 41)))
  0)
```

```
In function 'main__cps':
error: incompatible type for argument 2 of 'vec_hypush_ex'
note: expected 'int64_t' but argument is of type 'tur_tagged_t'
```

Both are the same mismatch at the two ends of a vector: the push that builds
the literal and the read that takes an element back out.

## Not the whole surface

Neither `vec-len` nor a user function over a vector trips it -- `(vec-len v)`
and `(sum [1 2 3])` inside a performing function both compile and run. It is
specifically the sites that move an *element* word: `vec_hypush_ex` on the way
in, the element read on the way out.

## Fix directions

The direct emitter boxes/unboxes a Saffron element at these sites; the CPS arm
(`src/passes/cps.c` and the `__cps` / `_pf<N>` emission in
`src/compiler/emit_*`) reuses the carrier-shaped helper signature without the
adaptation. Whatever `emit_expr.c` does for the element seam on the direct path
needs to happen on the CPS path too. A fixture pair -- one performing function
reading an element, one building a literal -- belongs beside
`tests/fixtures/docs-introducing-saffron-examples`.
