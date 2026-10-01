# The CPS backend passed a typed vector pointer into an int64 slot

**Severity: medium.** Invalid C, refused by clang and gcc 14. gcc 13 only
warns, but `run.sh`'s cc-warning ratchet fails on it:

```turmeric
(defn app [C] [f : (fn [C] C) v : C] : C (f v))
(defn k [A] [x : A] : A
  (app (fn [y : A] : A y) (let [v (vec-new)] (vec-push! v x) (vec-get v 0))))
(k 7.1)
;; error: incompatible pointer to integer conversion passing
;;        'tur_adt_Vec__float *' to parameter of type 'int64_t'
```

Found 2026-10-01 by the type fuzzer (seed 31337, the gbody crossing over a
`(Result FzB20 int)`), at every element type including `float`. A gcc-only probe
does not show it. **RESOLVED 2026-10-01.**

## Mechanism

`k` is CPS-colored because `app` calls its function parameter. In a spec the
CPS backend declares `(vec-new)`'s binder as the typed pointer
`tur_adt_Vec__float * v`, while the atom's elaborated kind is the int carrier.
The primitive call `vec-push-ex(int64_t v, ...)` is emitted from a raw let by
the direct emitter. Its pointer-into-carrier rule keys on the recorded C type
of a bare identifier, and the CPS binder declarations never recorded one. The
CPS argument ladder (`atoms_csv_call_typed_offs`) decided from the kind and
called the pointer "a plain int, no cast".

## Fix

- `emit_binder_decls` records each declared binder's C type
  (`emit_localvar_record_ctype`).
- The CPS argument ladder converts a pointer-typed atom into an `int64_t`
  parameter by its C type (`cps_atom_recorded_ptr`).

## Verified

`tests/fixtures/cps-typed-pointer-into-carrier-slot` at
`(Result B int)`, `float` and `(Option int)`, with a vec→map chain,
compiled under clang and `--interpret`. Suite 3465/0, turi 2508/0.
