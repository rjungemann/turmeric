---
status: resolved
severity: low
discovered: 2026-07-08
discovered-by: turi-interp-collections-libturi Part 3 (verify-first)
resolved: 2026-07-08
resolved-by: set.tur/map.tur self-declare their elaboration deps via (load ...)
area: interpreter elaborator (load-path dependency resolution for set.tur/map.tur)
---

# `(load "stdlib/set.tur")` / `(load "stdlib/map.tur")` failed to elaborate under the interpreter

Driving `(load "stdlib/set.tur")` or `(load "stdlib/map.tur")` through the
`turi_eval` C API (a bare libturi embedder) raised elaboration errors that do
**not** occur on the compiler's auto-loaded / `import`-based stdlib path:

```
stdlib/map.tur:*: warning [TUR-W0040]: unknown name 'hamt/iter-advance!'; will runtime-dispatch
stdlib/map.tur:*: error: if condition must be bool, got int
stdlib/map.tur:*: error [TUR-E0001]: function 'map-eq-loop' arg 1: expected ptr<void>, got int
stdlib/map.tur:*: error: typeclass 'Eq' is not defined
stdlib/set.tur:*: (the set-eq-loop / Eq analogues)
```

## Root cause (confirmed -- not the carrier bridge originally suspected)

The failure was **missing elaboration dependencies**, not a carrier-bridge gap.
`set.tur` and `map.tur` reference names that live in *other* stdlib modules:

- `hamt/iter-alloc` / `hamt/iter-advance!` / `hamt/keyeq` / `hamt/has-dynamic?`
  -- plain `defn`s (the name literally contains `/`) in `stdlib/hamt.tur`, used
  by the pure-Turmeric `set-eq-loop` / `map-eq-loop`.
- `Eq` / `Hash` typeclasses -- `stdlib/typeclass-eq.tur` / `typeclass-hash.tur`.

Neither file carried any `import`/`(load ...)` for these. The `tur` CLI's
interpret path builds one big ordered prelude
(`typeclass-eq ... typeclass-hash ... hamt, set, map, ...` -- see `cmd_eval_h`
in `src/main.c`) and elaborates it in a single `turi_eval` call, so every name
is co-visible; the compiled path auto-loads the same set. A libturi embedder
that `load`s `set.tur`/`map.tur` in isolation gets none of that: `hamt/*` is an
unknown name (elaborated as an `int`-typed runtime-dispatch fallback, so
`(if (hamt/iter-advance! ...) ...)` fails "condition must be bool, got int" and
`map-eq-loop`'s `ptr<void>` iterator arg gets an `int`), and `Eq` is undefined.

## Fix

`set.tur` and `map.tur` now self-declare their elaboration dependencies at the
top of the file, using the same `(load "stdlib/...")` convention many stdlib
modules already use (arrow.tur -> either.tur, async_file.tur -> fd.tur, ...):

```turmeric
(load "stdlib/typeclass-eq.tur")
(load "stdlib/typeclass-hash.tur")
(load "stdlib/hamt.tur")
```

An already-loaded stdlib module is deduped by the elaborator's load-visited set
(absolute auto-load path and cwd-relative `stdlib/...` load canonicalize to the
same key -- `src/compiler/elab_toplevel.c`), so under the CLI prelude / compiled
auto-load these forms are **no-ops**; they only do work when the module is
`load`ed standalone. The supported paths are unchanged.

## Verification

- Isolated `(load "stdlib/set.tur")` / `(load "stdlib/map.tur")` through
  `turi_eval` now elaborate cleanly, and the public Set/Map surface
  (`set-add`/`set-member?`/`set-count`, `map-assoc`/`map-get`/`map-count`)
  round-trips in a bare libturi embedder -- covered by the loaded-API section of
  `tests/turi/collections-embed.c` (ctest `tur_collections_embed`).
- `tests/run.sh`: 1978 passed, 0 failed (compiled path + codegen snapshots
  unaffected -- the `(load ...)` forms dedup under auto-load).
- `run-turi.sh`: 1450 passed, 4 pre-existing failures (unchanged from baseline).
