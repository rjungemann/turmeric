# `catch-unwind` around a panicking Saffron function that also makes a dynamic call crashes both engines

**RESOLVED 2026-09-26.** One correction to the filing first: **the
interpreter was never affected.** `tur run --interpret file` is not the
interpreter -- `run` compiles, and the flag went nowhere -- so the "both
engines" crash was the compiled one twice. `tur --interpret` printed `false`
for the repro all along.

The compiled crash was in the CPS function's direct entry wrapper
(`emit_cps_ir.c`, the direct->cps entry wrapper). A function that makes a
dynamic call is CPS-lowered, and when its return type rides a heap box (an
`any` is a two-word `tur_tagged_t`, a Tier-C value) the wrapper reads the
delivered value as `*(tur_tagged_t *)(__r)`. A body that panicked returns
before delivering anything -- its `if (tur_panicking) return 0;` -- so `__r`
was NULL and the read segfaulted before the panic could reach the
`catch-unwind` waiting for it. That is why the dynamic call only had to be
present, not run: it is what made the function CPS. The wrapper now reads
`__r ? <load> : (T){0}` for a boxed return, and the caller's own panic check
carries the panic on. Scalar returns are unchanged.

The **Related** note was a second, separate defect, fixed too: a Saffron
function whose body is only a `(panic ...)` kept its inferred `!` return,
which emits as `void`, while every caller had been elaborated against the
unannotated-Saffron `any` default -- `return fail(...)` of a void was the cc
error. A diverging unannotated body now takes the `any` signature like any
other (`elab_fns.c`, saffron-lang-plan S5/D3); the body needs no box.

Pinned by `tests/fixtures/saffron-catch-unwind-cps-panic`: panic on entry,
panic after dynamic self-calls, a panic-only body, and the no-panic control
whose value must survive the box read.

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
