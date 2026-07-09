# REPL: `#map{...}`, `#set{...}`, `[...]`, structs, and ADTs print as opaque integers / `#<struct T>`

**Severity:** low (usability paper cut; nothing is broken, results are just
unreadable at the prompt).

> **RESOLVED (Phase repl-show-collections).** `Show [Vec]` / `Show [Set]` /
> `Show [Map]` now ship, and the interpreter REPL renders any Show-having heap
> value through its instance. See the resolution note at the bottom of this
> file for exactly what landed, the design that was rejected on the way, and
> the follow-ups that remain open.

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

## Resolution (Phase repl-show-collections)

### What landed

1. **Collection Show instances** -- `Show [Vec]`, `Show [Set]`, `Show [Map]`,
   plus their pure-Turmeric loop helpers and a private `show-concat`, now live
   in a new **`stdlib/typeclass-show.tur`** (the Show class, its primitive
   instances, and `Show [ptr<void>]` moved there too). They render:
   - `(show (vec-of 1 2 3))` -> `[1 2 3]`
   - `(show (set-of 7 8 9))` -> `#set{7 8 9}`
   - `(show (hamt-of 1 10 2 20))` -> `#map{1 10 2 20}`
   Empty collections render `[]` / `#set{}` / `#map{}`.

2. **REPL auto-display** -- the reported symptom. `turi_try_show` was
   generalized (`turi_try_show_by_tag`, `src/turi/eval.c`) so a bare
   `TURI_INT` heap result whose elaborated type is a named ADT/struct/record
   (Vec, Set, Map, or any user `defstruct`/`defgadt` with a Show instance) is
   rendered through its Show instance. `extract_type_tag` now emits the type's
   head name (was `"unknown"`), and the REPL display path gained a fourth tier.
   Primitives and `ptr<void>` are skipped (the `ptr<void>` Show reads a pointer
   as a `result<T,E>`).

3. **Structs / ADTs (chunk 2 + 3)** -- `derive-show` already existed; the REPL
   generalization above means a struct with a derived (or hand-written) Show
   instance now prints as `Point { x = 3, y = 4 }` at the prompt instead of
   `#<struct Point>`. No new macro was needed.

### How the report's fix direction was adjusted

The report said "no compiler or REPL change is needed -- stdlib work." That
turned out to be incomplete:

- The interpreter's `turi_try_show` only fired for `TURI_STRUCT`; Vec/Set/Map
  are `TURI_INT` heap pointers there, so a stdlib instance alone was never
  consulted. Hence the `turi_try_show_by_tag` + `extract_type_tag` REPL work.
- A pure-Turmeric collection Show body needs a string concatenator that runs
  under the tree-walker; `str-concat` is inline-C with no native override, so
  a `show-concat` native was registered in `turi_register_collection_natives`
  (`src/turi/collections_native.c`).

**Rejected approach:** auto-loading the Show *class* (a `typeclass-show.tur`
stub in `g_stdlib_autoload_files`). It broke `derive-show` universally --
the macro-emitted `definstance Show [UserType]` orphans (TUR-E0013) against a
foreign Show module -- and added Show machinery to every compiled program's
prelude. Instead Show stays **on demand**: `typeclass.tur` loads
`typeclass-show.tur`, and the interactive REPL preloads just the Show slice
(`turi_env_preload_typeclasses`, `src/turi/preload.c`) -- deliberately *not*
the full `typeclass.tur`, whose inline-C `Error [ptr<void>]` `error-message`
would shadow the interpreter's async `error-message` builtin. `--interpret`
and the fixture worker keep Show opt-in, so fixtures that define their own
Show class or assert a missing-instance error are unaffected.

### Tests

- `tests/fixtures/show-collections/` -- compiled `(show ...)` over Vec/Set/Map.
- `tests/turi/repl-smoke.sh` -- REPL auto-display of Vec/Set/Map literals.
- Full suites green: `run.sh` (1992), `run-turi.sh` (1467), `run-flags.sh`
  (78), `run-stdlib-checks.sh` (32). No fixture-snapshot churn (the compiled
  auto-load list is unchanged).

### Known limitations / follow-ups (still open)

- **cstr / content-boxed elements** render their carrier pointer, not the
  string (e.g. `(show (vec-of "a" "b"))` -> `[<ptr> <ptr>]`). This is the same
  int64-carrier element-recovery limitation `Eq [Vec]` / `Eq [Set]` /
  `Eq [Map]` already carry; a real fix needs typed element recovery through
  the carrier and should be tackled alongside the Eq instances.
- **Keyword/`:Sym`-keyed maps** (`#map{:a 1}`) still cannot even be
  *constructed* under the interpreter (`map-empty-for`/Sym MapKey inline-C has
  no native override) -- a separate interpreter gap, unrelated to Show.
- **WASM REPL** does not yet preload the Show slice; `src/web/wasm_glue.c`
  would need to call `turi_env_preload_typeclasses` for parity with `tur repl`.
- **Auto-deriving struct Show** at `defstruct` time (so structs print without
  an explicit `derive-show`) was left out; `derive-show` remains opt-in.
