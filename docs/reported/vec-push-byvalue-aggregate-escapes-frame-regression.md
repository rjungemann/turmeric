# Regression: by-value aggregate vec element dangles again (escapes-frame fixture reads garbage)

**Severity:** High -- a previously-fixed, archived correctness bug has
regressed. A `Vec` of by-value aggregates built in a child frame reads back
garbage after the frame is reclaimed.

## Symptom

`tests/fixtures/vec-push-byvalue-aggregate-escapes-frame` stdout mismatch:

```
--- expected        +++ actual
-5                  +1073741824
-99                 +1073741824
```

Both `(.value a)` and `(.value b)` read `1073741824` (= 0x40000000) instead
of `5` and `99`.

## Repro

`tests/fixtures/vec-push-byvalue-aggregate-escapes-frame/input.tur`: build a
`(Vec (Option int))` in a child frame, return it, run a deep recursion to
clobber the reclaimed stack, then read the elements back:

```turmeric
(defn build-vec [] : int
  (let [vo (:: (vec-new) (Vec (Option int)))]
    (vec-push! vo (:: (some 5)  (Option int)))
    (vec-push! vo (:: (some 99) (Option int)))
    (:: vo :int)))

(defn main [] : int
  (let [vo (:: (build-vec) (Vec (Option int)))]
    (let [junk (clobber 4096 0)]           ;; overwrite build-vec's reclaimed frame
      (let [a (:: (vec-get vo 0) (Option int))
            b (:: (vec-get vo 1) (Option int))]
        (println (if (.is-some a) (.value a) -1))   ;; wants 5, gets 1073741824
        (println (if (.is-some b) (.value b) -1)))))) ;; wants 99, gets 1073741824
```

## Root cause

This fixture is the regression guard for the archived fix
`docs/archive/vec-push-byvalue-aggregate-element-stores-dangling-stack-address.md`.
That fix heap-promoted (malloc + copy) the by-value aggregate element at the
`vec-push!` carrier boundary so the element outlives the producing frame.

The identical failure mode is back: the pushed `Option__int` is again stored
as a pointer into `build-vec`'s stack slot, so after `build-vec` returns and
`clobber` overwrites the reclaimed frame, `vec-get`/`.value` reads reclaimed
stack -- hence the uniform `0x40000000` garbage. The heap-promotion at the
concrete->carrier `vec-push!` bridge is no longer firing for this shape.

Likely a fallout of by-value/HKT carrier-boundary churn
([[project_monomorphization_north_star]]): whatever handled the
concrete->carrier spill for `vec-push!` stopped heap-promoting the aggregate
element.

## Fix directions

- Re-locate the `vec-push!` carrier-boundary lowering that previously did
  malloc+copy for a by-value aggregate element; confirm why it now emits the
  `&stack_tmp` spill path again for `(Option int)`.
- The archived report has the original diagnosis and fix shape -- compare the
  current emit-c for this fixture against the archived "after the fix" C.
- Guard is already in place (this fixture); it must go green.
