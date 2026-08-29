# A struct-typed function passed to an arrow combinator returns garbage

**Severity: low** -- the arrow layer is documented as running on the erased
two-slot `int64` heap-pair carrier, and the working idiom (ascribe at the
edges) is a one-liner. But nothing rejects the struct-typed spelling, so the
failure is silent. Found while implementing `docs/archive/arrowloop-lazy-feedback.md`.

## Repro

```turmeric
(load "stdlib/arrow.tur")
(load "stdlib/tuple.tur")

(defn tur-step [t : (Tuple2 int int)] : (Tuple2 int int)
  (tuple2 (+ (tuple2-1st t) 1) (* (tuple2-1st t) 2)))

(defn main [] : int
  (let [lp (arrow-loop tur-step)]
    (println (lp 5)))          ;; want 6
  0)
```

Prints `-2305843009213693952` (was `0` before the lazy-feedback change --
either way, not `6`). Called directly, `(tur-step (tuple2 5 0))` is correct,
so the defect is entirely in the crossing.

## Root cause

Every arrow combinator dispatches its function argument through `TUR_APPLY1`,
which calls the target as `int64_t (*)(void *, int64_t)` -- see the closure
dispatch convention at the head of `stdlib/arrow.tur:23`, and the helpers that
use it (`stdlib/arrow.tur:248` onward). A `defn` declared to return
`(Tuple2 int int)` is emitted with the struct-by-value return ABI, not the
`int64` thunk ABI, so the call reads a return register the callee never set.

The parameter side happens to survive (the erased pair pointer arrives in the
right register); only the return value is garbage.

## Working idiom

Take and return the erased carrier, ascribing at the edges:

```turmeric
(defn tur-step [p : int] : int
  (let [t (:: p (Tuple2 int int))]
    (:: (tuple2 (+ (tuple2-1st t) 1) (* (tuple2-1st t) 2)) :int)))
```

This is what `tests/fixtures/arrow-loop-lazy-feedback` and
`tests/fixtures/arrow-loop-delay` do, and it is now documented under
"Writing the looped arrow" in `docs/guides/arrows-guide.md`.

## Fix directions

1. Cheapest: reject it. The arrow combinators take `^fat f` with no declared
   function type, so nothing checks the callee's ABI today. Giving them a
   declared `(fn [int] #fx{} int)` parameter type would turn this into a
   type error at the call site instead of garbage at runtime.
2. Fuller: teach the fat-closure shim to bridge a struct-returning callee into
   the `int64` thunk ABI (an sret trampoline), so the struct-typed spelling
   works as written.

## Guides to update when fixed

- `docs/guides/arrows-guide.md` ("Writing the looped arrow")
