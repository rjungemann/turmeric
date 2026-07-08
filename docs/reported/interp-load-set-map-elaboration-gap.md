---
status: open
severity: low
discovered: 2026-07-08
discovered-by: turi-interp-collections-libturi Part 3 (verify-first)
area: interpreter elaborator / carrier-bridge (load-path re-elaboration of set.tur/map.tur)
---

# `(load "stdlib/set.tur")` / `(load "stdlib/map.tur")` fail to elaborate under the interpreter

Driving `(load "stdlib/set.tur")` or `(load "stdlib/map.tur")` through the
`turi_eval` C API (or `tur --interpret` on a program that `load`s them
explicitly) raises elaboration errors that do **not** occur on the compiler's
auto-loaded / `import`-based stdlib path:

```
stdlib/map.tur:776:7:  error: if condition must be bool, got int
stdlib/map.tur:798:29: error [TUR-E0001]: function 'map-eq-loop' arg 1: expected ptr<void>, got int
stdlib/set.tur:*:      error: if condition must be bool, got int
stdlib/set.tur:*:      error: set-eq-loop arg 1: expected ptr<void>, got int
```

These fire **ahead of native dispatch**, so the
`turi-interp-collections-libturi` relocation (which makes the Vec/Set/Map/HAMT
native overrides available to every libturi env) does not make `Set`/`Map`
loadable via `load`. This is a **distinct root cause** from the main.c ->
libturi native-registration split -- it is an elaborator / carrier-bridge gap,
not a missing native.

## The supported path is fine

Real Set/Map usage in the `tur` binary reaches `set.tur`/`map.tur` through the
normal auto-load / `import` elaboration, which elaborates cleanly in **both**
interpret and compiled modes:

```turmeric
;; tests/fixtures/map-basic/input.tur, data-literal-set-basic/input.tur:
;;   tur run --interpret  => same output as  tur run  (compiled)
(let [m (:: (map-new) (Map int int))]
  (map-count (map-assoc (map-assoc m 1 10) 2 20)))   ; => 2
(set-count #set{1 2 3})                               ; => 3
```

and the relocated natives themselves round-trip through the pure `turi_eval`
embedder (see `tests/turi/collections-embed.c`: Vec int/float, Set
add/member/count/union, HAMT set/get/has/count).

## Root cause (suspected)

A `ptr<void>` <-> `int64` carrier equivalence (and an `int -> bool` coercion)
that the compiler's `emit_carrier_bridge` supplies during a fresh full-module
elaboration, but that the interpreter's `load`-time re-elaboration of an
already-lowered stdlib module does not reconstruct. `map-eq-loop` /
`set-eq-loop` take a `ptr<void>` HAMT-iterator handle and an `if` over a
`hamt/iter-advance!` result that has been lowered to an `int` carrier; on the
`load` path the elaborator sees the raw `int` carrier instead of the bridged
`bool` / `ptr<void>`.

## Fix directions

Scope this separately from the native relocation. Either (a) teach the
interpreter's `load` elaboration to apply the same carrier bridge the compiler's
auto-load path applies to `hamt/iter-*` results, or (b) document `load` of the
lowered stdlib as unsupported and point embedders at the auto-load / `import`
path (or at the native level directly, as the parity harness does).

Not a v1 blocker: the supported auto-load/import path works, and the native
level is reachable from embedders.
