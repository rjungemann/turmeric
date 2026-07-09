# REPL: `#map{...}`, `#set{...}`, `[...]`, structs, and ADTs print as opaque integers / `#<struct T>`

**Severity:** low (usability paper cut; nothing is broken, results are just
unreadable at the prompt).

## Summary

Evaluating a data-structure literal in `tur repl` prints an opaque integer
or a generic `#<struct T>` tag instead of the value's contents:

```
> #map{:a 1 :b 2}
140393012486144            ; or "#<struct Map>"

> [1 2 3]
#<struct Vec>

> (Point 3 4)
#<struct Point>
```

The same is true for `#set{...}`, ADT values, and user-defined structs.
Primitives (`:int`, `:bool`, `:cstr`, `:float`) and the two heap types with
built-in printers (`Pair`, `Cons`) print correctly.

## Root cause

The REPL already has a three-tier print-dispatch:

1. `turi_try_show(env, result)` -- lookup and call a `Show` instance if one
   exists for the value's type. `src/turi/eval.c:10335`.
2. `turi_show_result(env, result, type_tag)` -- built-in printer for known
   heap types (`Pair`, `Cons`).
3. `repl_print_value(result, use_color)` -- fallback default repr that emits
   `#<struct Name>` for `TURI_STRUCT` and the raw handle int otherwise.
   `src/turi/repl.c:303-332`, dispatched at `src/turi/repl.c:1319-1334`.

The dispatch is fine. The gap is in the stdlib: only `range` and
`range-bound` (`stdlib/range.tur`, `stdlib/range-bound.tur`) currently ship
`(definstance Show ...)`. `Map`, `Set`, `Vec`, `MutableMap`, and every
`defstruct` / ADT type fall through tier 1 straight to tier 3.

The `Show` class itself is already defined in `stdlib/typeclass.tur:96`:

```turmeric
(defclass Show [a] (show [x] : cstr))
```

## Fix directions

No compiler or REPL change is needed. This is stdlib work in three chunks:

1. **Collections** -- add `Show` instances that walk the underlying structure
   and format entries:
   - `stdlib/map.tur` -- `Show[(Map K V)]` requires `Show[K]` + `Show[V]`,
     emits `#map{k1 v1, k2 v2, ...}` by iterating the HAMT.
   - `stdlib/set.tur` -- `Show[(Set A)]` requires `Show[A]`, emits
     `#set{a b c}`.
   - `stdlib/vec.tur` -- `Show[(Vec A)]` requires `Show[A]`, emits
     `[a b c]`.
   - `stdlib/hamt.tur` (optional) -- raw HAMT `Show` in the same shape as
     `Map` for debugging.
   The HAMT already has iterator entry points used by `map/map` /
   `map/reduce`; reuse those rather than reaching into cells directly.

2. **Structs** -- a `derive-show` macro parallel to the existing
   `derive-json` slice (see `docs/archive/`-ish
   `project_p2a_derive_json_minimal`). Given
   `(defstruct Point [x : int y : int])`, it expands to a `Show[Point]`
   instance that prints `#<Point x=3 y=4>` (or a shorter positional form).
   Alternative: auto-emit a default `Show` at `defstruct` elaboration time
   and let users override with an explicit `definstance`. The macro route
   is cheaper and keeps the "opt in" property of the other derives.

3. **ADTs** -- same `derive-show` macro, dispatched over the ctor list, so
   `(Some 3)` prints `(Some 3)` and `(Cons 1 (Cons 2 Nil))` prints as a
   Cons chain (or degrades to the ctor form).

Ordering: (1) unblocks the reported case immediately with a few
straightforward instances. (2) and (3) can piggy-back on the `derive-json`
plumbing and land together.

## Notes

- The interpreter path calls `turi_try_show` unconditionally, so a stdlib
  `Show` instance is picked up in both `tur repl` and `--interpret` without
  any glue.
- The WASM REPL shares the same dispatch. Watch out for the
  inline-C-in-interpreter caveat tracked in
  `docs/reported/web-repl-repl-inline-c-native-gap.md`: any `Show`
  implementation whose body is inline-C will need the same `wk_register_*`
  treatment. Prefer pure-Turmeric bodies for the collection instances so
  they Just Work under WASM.
- Report filed 2026-07-09.
