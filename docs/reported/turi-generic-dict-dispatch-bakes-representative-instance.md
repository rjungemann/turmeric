# Generic-dict dispatch bakes the representative instance under `--interpret`

**Summary:** Under `tur --interpret`, a generic function that dispatches a
typeclass method through a dictionary-passing parameter over a `TY_APP`-bound
type variable (the GDE1/GDE2 "generic-dict" machinery) does **not** specialise
to the argument's real instance -- it bakes the *representative* (int-carrier)
instance and silently returns its answer. `tests/fixtures/gde4-generic-size-map`
prints `-1 / -1 / -1 / -1` instead of `0 / 1 / 2 / -1`: every `Map` argument is
routed to `Size [int]` (the representative, which returns `-1`) instead of
`Size [Map]` (which returns `map-count`). The compiled path is correct.

**Severity:** Medium-High. It is a **silent wrong-value miscompile** (rc=0, no
diagnostic) -- the most dangerous class -- bounded to the `--interpret` /
`tur repl` / WASM-REPL path. The compiled path that the fixture was written to
exercise is unaffected.

## Minimal repro

```turmeric
;; tests/fixtures/gde4-generic-size-map/input.tur (verbatim shape)
(defclass Size [a] (size [x] : int))

(definstance Size [int] (size [x] -1))          ; representative

(definstance Size [Map] [(MapKey K) (Eq V)]
  (size [x] : int
    ```c
    typedef struct { void *hamt; } tur_Map;
    tur_Map *m = (tur_Map *)(intptr_t)x;
    return tur_hamt_count(m->hamt);
    ```))

(defn count-it [^Size A] [x :A] :int (size x))   ; generic-dict driver

(defn main [] : int
  (let [m0 (:: (map-new) (Map cstr int))
        m1 (map-assoc m0 "a" 1)
        m2 (map-assoc m1 "b" 2)]
    (println (count-it m0))   ; expect 0, interpret prints -1
    (println (count-it m1))   ; expect 1, interpret prints -1
    (println (count-it m2)))  ; expect 2, interpret prints -1
  (println (count-it 42))     ; expect -1, prints -1 (correct)
  0)
```

```sh
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret tests/fixtures/gde4-generic-size-map/input.tur
#   => -1 / -1 / -1 / -1   (compiled `tur run` => 0 / 1 / 2 / -1)
```

## Observed vs. expected

- **Observed (interpret):** `count-it <map>` returns `-1` -- `Size [int]`'s
  constant. The generic-dict driver never specialises to `Size [Map]`.
- **Expected:** same as compiled -- `0 / 1 / 2 / -1`. The fixture's own header
  comment states this exactly: *"Without GDE1+GDE2 the generic count-it would
  bake Size [int] (the int-carrier representative) and return -1 for map
  arguments. With GDE1+GDE2 it correctly specialises to Size [Map]."* The
  interpreter is reproducing the pre-GDE (wrong) behaviour.

## Two independent divergences (the second is the already-tracked inline-C class)

1. **Generic-dict non-specialisation (this report, inline-C-independent).**
   `count-it [^Size A]` routes a `Map` argument to the representative
   `Size [int]` instance. The returned `-1` is `Size [int]`'s literal, *not* a
   mis-evaluated `Size [Map]` body -- if the driver had dispatched to
   `Size [Map]` and mis-run its inline-C we would see a pointer-shaped value, as
   the direct path does (below), not the clean `-1`. So the driver genuinely
   selects the wrong instance; this does not depend on the inline-C body at all.

2. **Direct dispatch mis-evaluates the inline-C body (separate, known class).**
   Calling `(size m2)` *directly* (bypassing the generic-dict driver) under
   `--interpret` dispatches to `Size [Map]` correctly but the inline-C
   `tur_hamt_count(m->hamt)` body is mis-evaluated by `try_exec_simple_inline_c`
   to the raw map pointer (e.g. `88235808189072`) instead of declining. This is
   the same inline-C-matcher-overclaim class tracked by
   [../archive/turi-inline-c-silent-miscompiles.md](../archive/turi-inline-c-silent-miscompiles.md)
   -- the matcher should return `turi_nil()` (clean "inline-C not supported"
   error) for a `tur_hamt_*`-calling body it cannot model, rather than guessing.

## Root cause (direction)

The GDE1/GDE2 specialisation that the compiled path performs -- picking the
real instance for a `TY_APP`-bound tyvar (`^Size A` where `A = Map cstr int`)
instead of the int-carrier representative -- is not mirrored in
`src/turi/eval.c`'s method-dispatch path. The interpreter resolves `size`
against the representative dictionary bound at the generic call boundary. The
fix is to teach the interpreter's typeclass-method dispatch to consult the
argument's runtime/elaborated `TY_APP` head (here `Map`) and select the
matching instance, the way the compiled generic-dict lowering does. (Pin the
exact resolution site before patching -- start from where `^Class A`
dictionary parameters are bound and where `size` is looked up under
`--interpret`.)

## Carve interaction

The fixture is inline-C-bound (the `Size [Map]` instance carries a ` ```c `
block), so under the eventual allowlist->denylist flip it would be skipped as a
TI7 carve-out. But the **generic-dict non-specialisation (divergence 1)** is
independent of the inline-C body -- it would still silently return the
representative answer for a pure-turi `Size [Map]` instance, so the carve does
not actually close this. (A clean pure-turi isolation is currently blocked
because a non-inline-C `Size [Map]` body that reads the count needs
`map-count`, which requires the fully-applied `Map K V` type and does not
typecheck against the instance's bare `Map` head in either path -- so the
inline-C body is the only writable form of this instance today. That typing
wrinkle is orthogonal and not part of this report.)

## Validation

After a fix: `ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret
tests/fixtures/gde4-generic-size-map/input.tur` matches `expected.stdout`
(`0 / 1 / 2 / -1`) -- or, if the inline-C body (divergence 2) is addressed
first by making the matcher decline, the fixture at least fails *cleanly*
(rc=1, "inline-C not supported") rather than printing `-1` silently. The
compiled path stays `0 / 1 / 2 / -1`.

## Status

Found while assessing the tractability of
[turi-map-set-hamt-interpreter-gap.md](turi-map-set-hamt-interpreter-gap.md)
(that umbrella's gaps -- the map/set/hamt natives, Tier B turi-closure
comparators, and non-int values -- are now all closed; `tib-map-turi-comparator`
passes under `--interpret`). `gde4-generic-size-map` is a *different* defect --
generic-dict dispatch over a `TY_APP`-bound tyvar, not the collection natives --
so it is filed separately here rather than against the map umbrella.
