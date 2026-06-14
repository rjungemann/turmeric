---
title: Why Path A doesn't specialize Map's Eq instance — it's intentional, not a defect
severity: documentation / design note
date: 2026-06-13
---

## Summary

Investigation triggered by the observation that `Eq Map`'s instance method
(`__inst_Eq_eq_qu_Map`) is emitted only as the bare int64-carrier function,
with no Path A `__spec__` clone, even though `Eq Vec` and `Eq MutableMap`
both get one.  Hypothesis going in: Map's 2-constraint instance
(`[(Eq K) (Eq V)]`) hits a Path A eligibility gate.  **That hypothesis is
wrong.**  Map reaches Path A correctly; the spec is suppressed downstream
because Map's C representation IS the int64 carrier in every instantiation,
by design.

## What actually happens

The Path A binding code at `elab_typeclasses.c:4055-4076` fires for any
non-HKT typeclass with one type parameter — Eq qualifies regardless of
the instance's constraint count.  Confirmed by instrumentation:

    [DBG] elab dispatch: fn=__inst_Eq_eq_qu_Map tc.n_type_params=1
                         is_hkt=0 obj_orig.kind=21 (TY_APP)

`emit_abi_register_call` receives the call with bindings attached, walks
through the Path A.1 generic-arg override gate at `emit_module.c:1053-1074`
— all conditions pass — and then evaluates the ABI-change check at
`emit_module.c:1075`:

    if (strcmp(type_c_name(generic_arg), type_c_name(arg_types[i])) != 0)
        abi_changes = true;

Both sides produce `"int64_t"`:

    [DBG] Map spec gate: generic_arg_cname=int64_t arg_types[i]_cname=int64_t

So `abi_changes` stays `false`, no per-instantiation spec is interned,
and the call dispatches through the bare int64-carrier method.

## Why both lower to int64_t

`stdlib/map.tur:22`:

    (defstruct Map [K V] (carrier :int))

Map is a **transparent int newtype** by construction.  Its single field is
`:int`, so `type_is_transparent_int_newtype((Map int int))` returns true
and `type_c_name` short-circuits to `"int64_t"` at `types.c:2145`.  The
generic-side `(Map K V)` lowers the same way — the type parameters K and V
don't materialize into Map's runtime layout; they exist only at elab time
for dispatch discrimination and for the K/V-typed accessor signatures.

This is **deliberate and load-bearing**:

- Every Map[K V] IS an `int64_t` (a heap pointer to a HAMT control block).
- Every Map operation is inline-C or delegates to one, so the value never
  goes through structural field access.
- A Path A clone like `Map__int__int { int64_t carrier; }` would be ABI-
  indistinguishable from the int carrier — emitting it would be pure
  churn with no register-class or layout consequence.

## How Vec and MutableMap differ

- `(defstruct Vec [A] (data :ptr<void>) (len :int) (cap :int))` — three
  fields, doesn't fit in an int64.  Concrete `Vec__int` IS a distinct C
  type; Path A specializes correctly.
- `(defstruct MutableMap [K V] (storage :ptr<void>))` — single
  `:ptr<void>` field, NOT `:int`.  `type_is_transparent_int_newtype` only
  fires for the literal `:int` field type, so MutableMap gets a real
  `MutableMap__cstr__int { void *storage; }` struct typedef.  Path A
  specializes it (4 bridge crossings in the audit confirm this).

## So what's the verdict on "do the same for Map"?

The Vec rewrite pattern doesn't apply to Map's Eq because:

1. Map gets zero Path A bridge crossings — there's no by-value-vs-carrier
   tension to retire, since the by-value representation IS the carrier.
2. The instance body `(eq? [x y] (map-eq-dynamic x y (fn [a b] (= a b))))`
   already passes x/y through the int carrier directly — no ascription
   bridge involved.
3. `try_synth_recursive_eq` at `elab_typeclasses.c:428-485` additionally
   intercepts Map dispatch for **content-keyed** equality on `:cstr` and
   struct keys, routing to `map-eq-k?` with a per-K MapKey comparator.
   This is correct for content equality and is not a Path A concern.

A pure-Turmeric Map Eq rewrite would still be a worthwhile language
exercise (it would retire the inline-C iteration in `map-eq-dynamic` /
`map-eq-raw?`), but it requires exposing HAMT iteration primitives in
pure Turmeric, which is the larger language work the
`tco-in-abi-specs-for-stdlib-iteration.md` plan called out as out of
scope for the current phase.

## What this changes

Nothing in code.  The Path A gating is correct.  The relevant doc files
(`docs/upcoming/tco-in-abi-specs-for-stdlib-iteration.md` and the
memory note on monomorphization north star) can be updated with the
"transparent int newtype" footnote so the next investigator doesn't
chase the same red herring.

## Related

- `docs/reported/m4-final-state-bridge-still-essential-for-collection-eq.md`
- `docs/upcoming/tco-in-abi-specs-for-stdlib-iteration.md` (Map deferred
  for HAMT iteration reasons; this finding confirms a second reason —
  no ABI delta exists to specialize on).
- `src/compiler/elab_typeclasses.c:4055-4076` — Path A binding.
- `src/compiler/emit_module.c:1053-1078` — Path A.1 + abi_changes check.
- `src/compiler/types.c:2143-2150` — `TY_APP` → transparent int newtype
  → `"int64_t"`.
