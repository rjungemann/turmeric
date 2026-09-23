# `catch-unwind` around a panicking Saffron function that also makes a dynamic call crashes both engines

**Severity: high** (SIGSEGV instead of a caught panic, on the compiled path AND
under `--interpret`). Found 2026-09-23 while landing proper-tail-calls T6;
pre-existing on `main` (reproduced with the T6 changes stashed).

## Repro

```turmeric
#lang saffron
(defn bl [self n] (if (= n 5) (panic "boom") (self self (- n 1))))
(defn main [] : int
  (let [r (catch-unwind (fn [] : any (bl bl 5)))]
    (println (ok? r)))
  0)
```

`tur run` and `tur run --interpret` both die with `Segmentation fault`.
`tur emit-c` exits 0, so it is the program (and the interpreter process), not
the compiler.

The dynamic call never even runs here -- `n` starts at 5 -- but it has to be
in the body. Two controls that print `false` on both engines:

- replace `(self self (- n 1))` with `n` (no dynamic call in the function);
- keep the dynamic call but call `(bl 1 5)`.

So the trigger is a function that is CPS-colored by a dynamic call
(`cps.c`: `has_indirect` on `EX_DYN_CALL`) panicking under a `catch-unwind`
whose thunk returns `any`. That the interpreter crashes too suggests a shared
cause above the emitters -- elaboration of the panic / catch-unwind pair in a
colored function -- rather than the CPS emitter alone.

## Related

A body that is only `(panic "boom")` fails differently -- `cc` rejects the
emitted C -- which may or may not be the same defect.

Not a T6 regression: T6's `tailcall-dyn-*` fixtures avoid panics for this
reason, and the typed-Turmeric panic paths (`tailcall-drop-glue-panic`,
`tailcall-mutual-panic`) are unaffected.
