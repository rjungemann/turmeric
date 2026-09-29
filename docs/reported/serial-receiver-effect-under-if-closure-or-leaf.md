# An escaping effect in a serial-shift context is refused under an `if`, in a capturing receiver, or in a leaf

**Severity: low.** A compile-time refusal (`TUR-E0706`), not a wrong answer;
`tur --interpret` runs all three.  The residue of
[serial-receiver-effect-cannot-reach-enclosing-handler](../archive/serial-receiver-effect-cannot-reach-enclosing-handler.md),
which made a NAMED receiver's escaping effect reach the handlers around the
reset by calling the receiver as a colored callee on the reset's own
continuation (`recv_outward`).

## The three shapes

```turmeric
(load "stdlib/serial.tur")
(defeffect Ask [] :int)
(defn page [env : cstr hole : int] : int (+ hole 1))
(defn asks [] : int (+ (perform (Ask)) 1))
(defn recv-e [k : serial-cont] : int (k (asks)))

;; 1. an `if` branch point in the context
(serial-reset (if c (page "" (serial-shift recv-e 0)) 5))

;; 2. a CAPTURING closure receiver
(serial-reset (page "" (serial-shift (fn [k : serial-cont] : int
                                        (k (+ m (asks)))) 0)))

;; 3. a context callee (leaf) the effect escapes
(defn page2 [env : cstr hole : int] : int (+ hole (perform (Ask))))
(serial-reset (page2 "" (serial-shift recv 0)))
```

Shape 1 is pinned by `tests/fixtures/errors/serial-shift-receiver-effect-escapes`.

## Why each is still refused

1. **`if`.** The outward lowering replaces the shift with a tail call whose
   continuation is the rest of the function.  With an `if` in the context the
   pure arm yields its value straight into the same rest, so the rest would
   have to be lifted once and reached from both arms -- the native serial
   lowering emits the pure arm inline instead.  `build_cloneable` refuses
   `recv_outward && saw_if` (cps_ir.c).
2. **Capturing closure.** The outward call names the receiver's `__cps` twin;
   a capturing lambda's twin takes its env first and is registered only for
   lambdas in the threadable set (`fn_sig_ok`).  Threading the closure value
   to that entry is the E2a fat-dispatch machinery, not wired here.  A
   non-capturing `fn` literal is lifted to a named global and works.
3. **Leaf.** A context callee runs when the continuation is RESUMED, not when
   it is captured -- possibly in another process, from bytes -- so its effect
   would need the resumer's handlers.  That is a question about what a
   marshalled continuation's effects mean, not a lowering gap.

## Fix directions

- (1): lift the rest as a resume frame before the branch point and deliver the
  pure arm into it (`dk_run(rest, pure)`), so both arms share one
  continuation.
- (2): call the closure through its env-taking `__cps` entry, the way the E2a
  heap join threads a fat fn value.
- (3): decide the semantics first -- probably "the resumer's handlers", which
  means `resume-cont!` would need to run the chain on the caller's `__kont`.
