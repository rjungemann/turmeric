# A capturing closure returned as `any` is never freed

**Severity: low-medium.** One closure env per call that returns a capturing
lambda, in any dynamic file (`#lang saffron`, `#lang r7rs`). No wrong answer,
but a Scheme program is made of exactly this shape -- every `lambda` a
procedure returns -- so a loop that builds closures leaks linearly.

**Pre-existing** at `main` (274f3cbb, measured with the r7rs-lang-plan work
stashed in a separate worktree build). Found at r7rs-lang-plan R9 while
triaging `tur_leak_check`.

## Repro

```turmeric
#lang saffron
(defn make-adder [k] (fn [x] (+ x k)))
(defn main []
  (let [add3 (make-adder 3)]
    (println (add3 4)))
  0)
```

Built with `tests/run-leak-check.sh`'s flags and run with every LeakSanitizer
root disabled (`LSAN_OPTIONS=use_globals=0:use_registers=0:use_stacks=0:use_tls=0`),
the 32-byte env allocated in `make_hyadder` is reported. The typed twin --
`(defn make-adder [k : int] : (fn [int] int) (fn [x : int] : int (+ x k)))` --
reports nothing: the direct emitter frees the env at the `let`'s scope end.
Under default LSan options the dynamic program usually passes too, because a
stale copy of the pointer survives on the stack or in a global and LSan counts
the block reachable. That is luck, not ownership.

## Why it surfaced now

`tests/fixtures/tailcall-dyn-leak` has carried this leak (its `make-counter`
closure) and a second one -- a vector it never freed -- since it was written,
and passed on stale-pointer luck. r7rs-lang-plan R6 changed the dynamic call's
emitted C (the variadic check), which moved those words, and the gate went
red with 168 bytes. R9 frees the vector in the fixture (a compiled Vec is
released with `vec-free`, per docs/guides/gc-guide.md) and marks the fixture
`known-leak` against this report for the closure.

## Where to look

The `let` scope-end drop of a closure env is keyed on the binding's static
type being a fat closure; a dynamic file's binding is `any` (the lambda was
widened on return), so no drop is emitted. `any-widen-stored-in-an-adt-field-has-no-owner`
tracks the same family for by-value payloads.

## Fix directions

- Drop a closure-typed `any` at scope end the way a typed fat closure is
  dropped, when the binding does not escape (the same escape facts the typed
  path uses).
- Or give the `any` box of a closure an owner at the widen, so the box's drop
  releases the env.

Delete `tests/fixtures/tailcall-dyn-leak/known-leak` when this is fixed; the
leak gate turns red if the leak comes back.
