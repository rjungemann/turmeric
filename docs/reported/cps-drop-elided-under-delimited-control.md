# Heap ADT drop is elided when its owning function contains delimited control

**Severity:** low-to-medium (heap leak of a whole collection; not a crash or
miscompile). Keeps `requires.no-leak-check` on `cps-backend-heap-adt-return`.

## Summary

A heap-backed ADT (e.g. a `(Vec int)`) that is created and returned by a
function is normally reclaimed by the drop machinery when the value goes out of
scope. But when the *same* function body also contains delimited control
(`handle`/`resume`, and presumably `reset`/`shift`), the drop is not emitted and
the collection -- its header struct plus its backing data array -- leaks.

The tell is that removing only the effect leaves the code leak-clean: the drop
insertion is being lost across the CPS transformation, not because the Vec is
inherently unmanaged.

## Minimal repro

Leaks (effect present):

```turmeric
(load "stdlib/vec.tur")
(defeffect E [] :int)
(defn g [] : int (perform (E)))
(defn mkvec [] : (Vec int)
  (let [n (handle (g) (E [] k) (resume k 3))]
    (let [v (vec-new)]
      (vec-push! v n)
      v)))
(defn main [] : int (println (vec-len (mkvec))) 0)
```

LSan: direct `24 byte(s)` from `vec_new` + indirect `32 byte(s)` from
`vec_push!`, both attributed to `mkvec`.

Clean (same shape, no effect) -- proves the drop works absent delimited control:

```turmeric
(load "stdlib/vec.tur")
(defn mkvec [] : (Vec int)
  (let [v (vec-new)] (vec-push! v 3) v))
(defn main [] : int (println (vec-len (mkvec))) 0)
```

## Root cause (suspected)

The owning function `mkvec` is CPS-transformed because it reaches a `handle`.
The RC/drop pass that would inject the Vec's release either runs before the CPS
split and its inserted drop is dropped when the body is re-threaded into
continuation frames, or runs after and does not see the escaping-then-dropped
Vec across the continuation boundary. Allocation sites are in the compiled
program (`vec_new__spec__...`, `vec_push!`); the missing free is the codegen
concern -- to be pinned to the drop-insertion pass interaction with
`src/compiler/emit_cps_ir.c`. Not yet root-caused to a single file:line.

## Fix directions

1. Determine where the drop for a returned-and-then-dropped heap ADT is inserted
   for a non-colored function, and ensure the same insertion survives (or is
   re-run after) the CPS transformation of a colored function.
2. Confirm scope: does this also swallow drops for heap ADTs consumed *inside* a
   colored function (not just returned)? A borrow/consume across a `resume`
   (multi-shot) needs the drop count to match the number of live copies.
3. Interim: `cps-backend-heap-adt-return` keeps `requires.no-leak-check`.
