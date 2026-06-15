---
title: Carrier-returning instance method + ascribed (Result A cstr) leaks carrier int when A is a non-int primitive (bool)
severity: medium -- ergonomics gap blocks a whole shape of typeclass instance; not a silent miscompile at the typing layer (TUR-E0001 fires), but attempting to fix the typing reveals a deeper silent-miscompile at the ABI layer
status: resolved 2026-06-15 (direction 2 -- field-by-field bridge reconstruction)
discovered: 2026-06-15
investigation: 2026-06-15
resolved: 2026-06-15
---

## Resolution (2026-06-15)

Direction 2 from "Proposed fix directions" landed: `emit_carrier_bridge`
now reconstructs `Result`/`Option` field-by-field from the canonical
`tur_result_box_t` / `tur_option_t` carrier layout when crossing
CK_CARRIER -> CK_CONCRETE, and the two M4c/M5 return-deref sites in
`emit_fns.c` were rewired to route through the same bridge instead of
emitting `*(T *)(intptr_t)x` inline. The elab side
(`call_wrap_reinterpret`) also accepts size-mismatched integral
reinterprets so the typing layer agrees that
`(ok-val (:: ... (Result bool cstr)))` is `bool`.

**Files changed**: `src/compiler/elab_call.c`, `src/compiler/emit_core.c`,
`src/compiler/emit_expr.c`, `src/compiler/emit_fns.c`.

**Validation**: the spice-side `decode-bool.tur` flips FAIL->PASS;
`decode-primitives.tur` and `derive-decode-struct.tur` stay green; the
turmeric-side in-tree regression lives at
`tests/fixtures/decode-bool-carrier-instance-ascription/`. Full
`tests/run.sh` shows no new failures versus the pre-fix baseline (the
one snapshot delta -- `inline-c-typed-result-option/expected.c` -- was
regenerated; runtime output unchanged).

The bridge is still interim. M4
(`docs/upcoming/m4-typeclass-per-method-abi-plan.md`) will retire
`emit_carrier_bridge` entirely; when it lands, the field-by-field
branch added here is one of the deletions that drop out for free.

---

# Carrier-instance ascription leaks carrier `int` when `A` is `bool`

## One-line summary

When a typeclass instance method body returns the int64 `tur_ok` carrier
and the call site is ascribed `(:: (decode ...) (Result A cstr))`,
`(ok-val ...)` resolves to the **carrier** type `int` instead of the
substituted `A`. For `A = int` this is identity and looks fine; for
`A = cstr` it coerces (pointer width); for `A = bool` it fails type
check.

## Minimal repro

```turmeric
(defmodule t (export)

(defclass Decode [a] (decode [doc : int] : (Result a cstr)))

(definstance Decode [bool]
  (decode [doc]
    ```c return tur_ok((int64_t)(doc == 1)); ```))

(defn show-bool [b : bool] : void
  (if b (println "true") (println "false")))

(defn main [] : int
  (let [r (ok-val (:: (decode 1) (Result bool cstr)))]
    (show-bool r))
  0))
```

Run:

```
$ ./build/tur run /tmp/test_bool_carrier.tur
t.tur:14:16: error [TUR-E0001]: function 'show-bool' arg 1: expected bool, got int
```

The error fires at the elaborator. The user wrote `A = bool`; the
elaborator gave `ok-val` the carrier `int` instead.

## Observed vs. expected

- **Observed (typing layer)**: `(ok-val (:: (decode 1) (Result bool cstr)))`
  has type `int` per the elaborator.
- **Expected (typing layer)**: it should have type `bool`, the
  substituted `A`.
- **Observed (ABI layer, see investigation below)**: even with the
  typing patched, the runtime value is wrong -- the consumer-side
  carrier->concrete bridge reads padding bytes of `tur_result_box_t` as
  if they were the `bool ok_val` field of `Result__bool__cstr`.

## Why the existing fixes do not cover this

Two adjacent landings looked like they should have fixed it but do not:

1. **PR #385 / `instance-method-return-carrier-bridge.md`.** That fix
   targeted the *by-value producer* side: instance bodies that lower to
   `ok__spec__*` via `make-struct` (so the tail already emits the
   by-value struct). The plan's "Status: shipped" note explicitly says
   `decode-bool` is a follow-up "spice-side fixture commit"; on
   re-execution the test is still red, which means the planning step
   that triaged `decode-bool` as the same root cause was wrong.

2. **M2 `#{Construct}` / `make-struct`.** Only applies when the
   instance body constructs the Result by value. A primitive payload
   (`bool`) returned through inline-C `tur_ok((int64_t)1)` never enters
   the by-value clone path; the dispatch shim still returns int64
   carrier and `ok-val` follows the carrier type, not `A`.

So this is the *carrier-source* sibling of the seam that PR #385 closed
on the *by-value-source* side -- different producer, same consumer
ascription, different fix.

## Root cause (investigation, 2026-06-15)

Originally I suspected this was a single bug in
`src/compiler/elab_call.c:269` (`call_wrap_reinterpret`) -- which it
partly is, but fixing it surfaces a second, deeper bug. Both bugs are
required to make the bool repro work end-to-end.

### Bug 1 (typing, elab_call.c) -- size-mismatched reinterpret is dropped

`elab_call.c:3622-3645` already handles polymorphic-result calls whose
result kind is `TY_TYVAR`: it sets the call's elab type to `TYPE_INT`
(the carrier) and wraps the call in `EX_REINTERPRET` to the substituted
type when the substitution resolves to a scalar. The wrap goes through
`call_wrap_reinterpret`, which **bails when source and destination sizes
differ**:

```c
int src_size = type_size_bytes(source_kind);
int dst_size = type_size_bytes(target_kind);
if (src_size <= 0 || dst_size <= 0 || src_size != dst_size) return inner;
```

For `int` (8) -> `cstr` (8): same size, wrap succeeds. For
`int` (8) -> `bool` (1): `src_size != dst_size`, wrap silently
skipped, the call's elab type stays `int`. That's the TUR-E0001 message.

A first-pass fix is small: allow size-mismatched reinterpret when both
kinds are integral (`bool`, `int8`/`16`/`32`/`64`, `uint8`/`16`/`32`/`64`),
and at emit (`emit_expr.c:1283`) lower size-mismatched integral pairs
as plain C casts (`(bool)x`, `(int8_t)x`) instead of the same-size union
trick. I prototyped this, and the elab error disappears -- but the next
bug is now visible.

### Bug 2 (ABI, emit_carrier_bridge) -- carrier and by-value layouts disagree

Once typing is patched, the call site instantiates `ok_val` into the
M2 by-value spec `ok_val__spec__bool_Result__bool__cstr` which takes a
`Result__bool__cstr` *by value*. The emitter has to bridge the
instance's int64 carrier return to that by-value struct, and does so via
`emit_carrier_bridge` (`src/compiler/emit_core.c:2422`). For a struct
sink the bridge emits:

```c
(*(Result__bool__cstr *)(intptr_t)(__inst_Decode_decode_bool(...)))
```

That deref assumes the carrier pointer points at a buffer **shaped like
`Result__bool__cstr`**. It doesn't:

| Layout (offsets, x86-64 SysV) | `tur_result_box_t` (what `tur_ok` allocates) | `Result__bool__cstr` (what the spec consumes) |
|---|---|---|
| 0 | `bool is_ok`        | `bool is_ok`        |
| 1 | (padding to align int64 at 8) | `bool ok_val`       |
| 8 | `int64_t ok_val`     | `const char *err_val` |
| 16 | `int64_t err_val`    | -- (end of struct)  |

For `A = int`, `Result__int__cstr` has the same layout as
`tur_result_box_t` (int64 `ok_val` at offset 8), so the wrong deref
happens to read the right bytes -- the bug is invisible.

For `A = bool`, `Result__bool__cstr.ok_val` lives at offset 1 (no
padding before it), so the deref reads a padding byte of
`tur_result_box_t` (= 0 from `malloc`-then-set-`is_ok`) and reports
`false` even when `tur_ok((int64_t)1)` was called. I confirmed this by
prototyping Bug 1's fix and running the repro: compiles cleanly, prints
`false` for `decode 1`. Same class of latent miscompile applies to any
sub-int primitive payload (`int8`, `int16`, `int32`, `float32`).

The asymmetry is that the *producer* side (carrier-source) uses the
universal `tur_result_box_t` shape (always int64 fields) while the
*consumer* side (the by-value spec) expects the concrete
`Result__T__U` shape (native field widths). The two only agree when `T`
and `U` are both int64-shaped.

## Proposed fix directions

This is no longer a one-session interim bridge. The available
directions all have non-trivial scope:

1. **Align the by-value struct layout to the carrier**: force
   `Result__T__U.ok_val` (and `Option__T.value`, etc.) to be `int64_t`
   regardless of `T`, with narrowing at field-access sites. Mechanical
   but touches every `make-struct Result` lowering and every
   `(.ok-val r)` field read; needs a fixture-snapshot regen.
2. **Reconstruct field-by-field in the bridge**: in
   `emit_carrier_bridge`, when the source is a carrier-shaped buffer
   (i.e., the producer is an inline-C `tur_ok` / `tur_err` body) and the
   sink is a parameterized `Result__T__U` / `Option__T`, emit a struct
   literal that reads each field from the canonical carrier layout and
   casts it to the concrete field type. Requires a way to tell
   carrier-shaped sources apart from already-concrete sources (the M2
   spill path at `emit_core.c:2486-2493` produces a concretely-shaped
   buffer; both paths share the same `emit_carrier_bridge` entry today).
3. **Fold into M4 (`m4-typeclass-per-method-abi-plan.md`)**: under M4,
   dict slots hold per-instance typed function pointers and instance
   bodies for declared by-value returns get rewritten to return the
   concrete struct directly -- the broken deref disappears entirely. If
   M4 is on the near horizon, direction 3 is the cheapest because the
   intermediate state from directions 1 or 2 has to be removed anyway.

Direction 1 is the most strictly-correct standalone fix; direction 2 is
the most surgical; direction 3 is the cleanest end-state. Pick based on
M4 timing.

## Validation when fixed

- `/tmp/test_bool_carrier.tur` above compiles **and prints `true`**.
- `../turmeric-spices/spices/json/tests/decode-bool.tur` flips
  FAIL->PASS (currently red at TUR-E0001:32:13).
- `../turmeric-spices/spices/json/tests/decode-primitives.tur` stays
  green (no regression on the `A = int` / `A = cstr` happy paths --
  these accidentally work because their carrier and concrete layouts
  match; a layout-aligning fix must preserve that).
- `tests/fixtures/instance-method-return-carrier-bridge/` stays green
  (the by-value-producer regression must not regress while we add the
  carrier-source coverage).
- Add a new turmeric-side fixture mirroring the bool repro above so the
  carrier-source seam has in-tree regression coverage without depending
  on the json spice. The fixture must assert the runtime output, not
  just compilation -- the silent-miscompile under Bug 2 would be missed
  by a compile-only fixture.

## What I tried this session

I prototyped Bug 1's fix in `call_wrap_reinterpret` + `EX_REINTERPRET`
emit (allow integral size mismatch via plain C cast). The compile error
disappeared, but the runtime output was wrong (`false` for
`tur_ok(1)`), exposing Bug 2. Both changes were reverted; neither
landed. The bug report itself was updated to reflect the deeper
finding.

## Cross-references

- `docs/upcoming/instance-method-return-carrier-bridge.md` -- the plan
  whose execution surfaced this gap. Its "spice-side follow-ups" note
  needs to be amended to point at this report (the spice-side fixture
  cannot land green until this is fixed).
- `docs/upcoming/m4-typeclass-per-method-abi-plan.md` -- the
  structural successor; M4 retires this whole class of bridges. This
  report is a candidate to fold into M4 rather than fix standalone,
  depending on M4 timing.
- `docs/upcoming/end-to-end-monomorphization-plan.md` -- parent plan;
  the long-term direction is to remove the int64 carrier ABI entirely,
  at which point Bug 2 is structurally impossible.
