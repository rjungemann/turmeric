---
title: Eq[Map] cannot expose a typed `(Map__K__V *)` consumer because Map is a transparent int newtype
category: Bug report / expressiveness gap -- ABI / Codegen (end-to-end monomorphization, typed-collection-eq-consumers-plan step 7)
severity: Low. Not a miscompile. `Eq[Map]` is correct and fast (pure-Turmeric
  `map-eq-driver`/`map-eq-loop` over the HAMT cursor); it simply dispatches via
  the int64 carrier `__inst_Eq_eq_qu_Map` instead of a typed by-value spec. The
  only cost is that the producer-slice plan's Map half has zero typed consumers
  to point at, so typing Map producers buys nothing until this is addressed.
status: RESOLVED. `Map` is now a non-transparent `:heap` struct
  (`(hamt :ptr<void>)`), so `(Map K V)` monomorphizes to a real `Map__K__V` and
  lowers to a typed pointer in every pure-Turmeric C signature. `Eq[Map]` now
  dispatches via the typed by-value spec
  `__inst_Eq_eq_qu_Map__spec__bool_Map__int__int___Map__int__int__`, and typed
  `(Map int int)` consumers receive a `Map__int__int *` with no int->pointer
  relabel. See "Resolution" below.
---

# Resolution (this session)

Three coordinated pieces -- the producer and consumer flipped together, exactly
as the fix directions predicted, plus one extra piece the directions did not
anticipate:

1. **`stdlib/map.tur` -- Map is now a non-transparent heap struct.**
   `(defstruct Map :heap [K V] (hamt :ptr<void>))` (was `(carrier :int)`). The
   in-C field layout is unchanged (`struct { void *hamt; }`), so every inline-C
   body that casts the carrier to that anonymous struct stays byte-compatible.
   The seven heap-returning inline-C producers (`map-new`, `map-wrap`,
   `map-assoc-eq-o`, `map-dissoc-eq-o`, `map-assoc-eq`, `map-dissoc-eq`,
   `map-merge`) now return through `(__TUR_RET__)(intptr_t)` instead of
   `(int64_t)(intptr_t)`, so each mints its typed producer spec.

2. **`src/compiler/emit_module.c` -- `Map` added to `type_is_heap_vec`'s
   allow-list** (`{"Vec", "Map", "MutableMap"}`).

3. **`src/compiler/emit_module.c` -- the float/cstr carrier-forcing block now
   handles the degenerate multi-param declared type.** This was the piece the
   fix directions missed. A multi-type-param collection (`(Map K V)`,
   `(MutableMap K V)`) is declared as a *degenerate* `TY_APP` -- a spineless
   shell whose head `app.fn` is NULL -- so `type_is_heap_vec(fd->param_types[i])`
   could not recover the `Map` StructDef and the forcing block never fired. The
   result was that `map-assoc-eq-o`'s `val :V` slot monomorphized to `double`
   for a `(Map int float)`, and the inline-C `(void *)(intptr_t)val` numerically
   truncated `0.5 -> 0` (caught by `tce3-map-cstr-val`). The fix keys the
   heap-collection slot test on the RESOLVED arg/result type as a fallback,
   guarded by `decl.kind == TY_APP` so a bare-tyvar generic (`some`/`ok` over
   `A`) is still excluded. (`MutableMap` did not hit this because its accessors
   stay on the int64 carrier base -- only its producer `mutmap-new` is typed --
   whereas Map's `map-assoc-eq-o` accessor gets a typed spec from the now-typed
   call site.)

**Validation.**
- `tests/fixtures/tce3-map-cstr-val` (`Map int float` / `Map int cstr`) passes:
  `0.5` round-trips, zero warnings.
- New fixture `tests/fixtures/map-typed-consumer` exercises a typed
  `(Map int int)` flowing through `size-of` (emitted `Map__int__int *`),
  `Eq[Map]` dispatching via `__inst_Eq_eq_qu_Map__spec__...`, and a float-valued
  map -- 0 warnings.
- The minimal repro emits the typed spec; `grep '(Map__[A-Za-z0-9_]* *)(intptr_t)'`
  is now non-empty.
- `bash tests/run.sh`: 1824 passed, 0 failed (88 snapshots regenerated for the
  `Map` struct field rename `int64_t carrier` -> `void * hamt`).
- `bash tests/run-turi.sh`: interpreter-parity-neutral -- the identical 32
  pre-existing by-value-migration failures, no new ones.

The original finding is retained below for the paper trail.

---

# Eq[Map] stays on the int64 carrier -- root cause is Map's representation, not #364

## Summary

`docs/upcoming/v1/typed-collection-eq-consumers-plan.md` step 7 expects typed
`(Map__[A-Za-z0-9_]+ *)(intptr_t)` consumers to appear once `Eq[Map]` is
rewritten to project its receiver by value. The rewrite landed (Phase
TCO-Eq-MapSet: `map-eq-driver` takes `(Map K V)` by value, `map-eq-loop`
walks the HAMT cursor), but **no typed Map consumer ever appears**. `Eq[Map]`
still dispatches via the int64 carrier `__inst_Eq_eq_qu_Map(int64_t, int64_t)`.

The plan (and an earlier pass at this report) guessed the cause was the `#364`
multi-param instance-spec promotion gate. That is **wrong**. `Set` and
`MutableMap` also have two/one type params and *do* get typed consumers.

## Root cause

`Map` is declared:

```turmeric
(defstruct Map :heap [K V] (carrier :int))
```

A parametric `:heap` struct with a **single `:int` field** is a *transparent
int newtype*: `type_is_transparent_int_newtype` (src/compiler/types.c) returns
true for it, and `type_c_name`'s `TY_APP` / `TY_STRUCT` cases then lower
`(Map K V)` to `int64_t` in **every** C signature -- pure-Turmeric and inline-C
alike. map.tur's own module header states this explicitly:

> Map is declared as a *transparent int newtype* ... so (Map K V) is emitted
> as int64_t in every C signature ... while the type checker still tracks K
> and V.

Consequently there is no `Map__int__int` C struct to take the address of. The
"by-value receiver" the Path-A spec would project is literally an `int64_t`,
so `emit_abi_intern_spec` has nothing to specialize and dispatch stays on the
carrier `__inst_Eq_eq_qu_Map`. Constraint count (`(Eq K) (Eq V)` vs `(Eq V)`)
and `(:: m (Map K V))` ascription make no difference -- verified empirically.

The three collections that *do* expose typed consumers are all
non-transparent (real monomorphized structs):

| Type | defstruct | transparent? | typed consumer |
|---|---|---|---|
| `Map` | `[K V] (carrier :int)` | **yes** | none (`int64_t`) |
| `Set` | `[A] (hamt :ptr<void>)` | no | `(Set__A *)(intptr_t)` |
| `MutableMap` | `[K V] (storage :ptr<void>)` | no | `(MutableMap__K__V *)(intptr_t)` |
| `Cons` | `[A] (head A) (tail :int)` | no | `(Cons__A *)(intptr_t)` |

## Minimal repro

```turmeric
(defn main [] : int
  (let [m1 (map-assoc (:: (map-new) (Map int int)) 1 10)
        m2 (map-assoc (:: (map-new) (Map int int)) 1 10)]
    (println (eq? (:: m1 (Map int int)) (:: m2 (Map int int)))))  ; true
  0)
```

`emit-c` shows `__inst_Eq_eq_qu_Map(m1, m2)` (carrier), and
`grep '(Map__[A-Za-z0-9_]* *)(intptr_t)'` is empty. Swap `Map`->`Set`
(single param) or compare against the `mutmap-eq` fixture and the typed
`(Set__int *)` / `(MutableMap__int__int *)` consumer appears.

## Fix directions

Make `Map` a non-transparent heap struct so it monomorphizes to a real
`Map__K__V`, mirroring `MutableMap`:

```turmeric
(defstruct Map :heap [K V] (hamt :ptr<void>))   ; was (carrier :int)
```

map.tur's inline-C already casts the carrier to `struct { void *hamt; } *`, so
the in-C field layout is unchanged; the work is in the *signature* lowering:
every pure-Turmeric `(Map K V)` parameter/return flips from `int64_t` to
`Map__K__V *`, which ripples through `map-eq-driver` and the typed accessors
and regenerates every Map-touching snapshot. This is the same shape as the
MutableMap producer-typing fix
(`docs/archive/history/mutmap-multi-param-producer-typing-blocked.md`) and
belongs in `map-set-typed-pointer-producer-slice-plan.md`'s Map slice -- the
producer (`map-new`'s `(int64_t)(intptr_t)` -> `(__TUR_RET__)(intptr_t)` plus
adding `Map` to `type_is_heap_vec`) and the consumer have to flip together, or
the int->pointer mismatch the MutableMap report describes reappears.

Until then, `Eq[Map]` is correct on the carrier path and this is a deferred
expressiveness gap, not a defect blocking v1.
