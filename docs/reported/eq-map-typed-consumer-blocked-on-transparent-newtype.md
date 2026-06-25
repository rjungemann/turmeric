---
title: Eq[Map] cannot expose a typed `(Map__K__V *)` consumer because Map is a transparent int newtype
category: Bug report / expressiveness gap -- ABI / Codegen (end-to-end monomorphization, typed-collection-eq-consumers-plan step 7)
severity: Low. Not a miscompile. `Eq[Map]` is correct and fast (pure-Turmeric
  `map-eq-driver`/`map-eq-loop` over the HAMT cursor); it simply dispatches via
  the int64 carrier `__inst_Eq_eq_qu_Map` instead of a typed by-value spec. The
  only cost is that the producer-slice plan's Map half has zero typed consumers
  to point at, so typing Map producers buys nothing until this is addressed.
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
