---
title: Non-scalar value support in the CPS-IR-to-C backend -- Plan
category: Planning
description: The CPS-IR-to-C backend (cps-ir-to-c-backend-plan) threads every value through the DK continuation machine as a single int64_t slot and declares every let-binder int64_t, so tk_scalar / atom_ok / fn_sig_ok reject anything that is not a machine-integer scalar. A colored function that touches a float, string, pointer, ref, or struct/ADT therefore falls back whole. Phase C5 measured this: after closing type ascription, the remaining CT_UNSUPPORTED blockers in the effect+delimited corpus are ADT/struct forms (EX_GET_FIELD, EX_MAKE_STRUCT, EX_DEFAULT_OF) and strings (EX_CSTR_LIT) -- all non-scalar. This plan widens the backend from "int64 scalars only" to the full set of Turmeric value representations by tiering values on how they cross the one-int64 DK slot boundary, keeping the whole-function fallback for the cases that genuinely do not fit.
---

# Non-scalar value support in the CPS-IR-to-C backend -- Plan

## Why this exists

The CPS-IR-to-C backend (`src/compiler/emit_cps_ir.c`, `--enable=cps-backend`)
lowers a colored function's ANF/CPS IR to C by threading continuations through
the DK multi-prompt machine. Two simplifying assumptions run through it:

- **Every value is an `int64_t`.** `emit_binder_decls` declares *every*
  let-binder `int64_t`; `atom_str` materializes atoms as `int64_t`; the DK value
  slot (`dk_run`/`dk_frame`/`dk_invoke`/`dk_perform` all pass one `intptr_t`) is
  one machine word.
- **Only machine-integer scalars are representable.** `tk_scalar` = `TY_INT` /
  `TY_BOOL` / `TY_INT64`; `atom_ok` rejects `CA_OTHER` (float/string/etc.);
  `fn_sig_ok` requires scalar params and return.

The consequence is coarse: a colored function whose body so much as names a
`float`, a string literal, a `ref`, or an `Option` **falls back to the
direct-style / fiber path as a whole**. Phase C5 quantified the cost by tallying
`<unsupported>` reasons across the 120-fixture effect + delimited corpus:

| form | count | why unsupported |
| --- | --- | --- |
| `EX_GET_FIELD` (struct/ADT read) | 1077 | non-scalar value |
| `EX_MAKE_STRUCT` | 358 | non-scalar value |
| `EX_DEFAULT_OF` | 357 | non-scalar value |
| `EX_CSTR_LIT` (strings) | 42 | non-scalar value |

Every remaining top blocker is a non-scalar value. This plan removes the
"int64 scalars only" ceiling.

## The core constraint: the DK slot is one machine word

The DK machine threads exactly one `intptr_t` per delivered value. That word is
the *only* place a value must be a fixed width; everywhere else (C params, C
locals/binders) a value can keep its real C type. So the design reduces to a
single question -- **how does each value representation cross the one-word DK
slot?** -- applied at a small, enumerable set of boundaries:

1. `CT_APPCONT` to `KK_RET` -> `return dk_run(k, <slot>)` (value into the slot).
2. A lifted continuation / shift / perform frame's incoming value param
   (`intptr_t value`) -> a real-typed local (value out of the slot).
3. A lifted frame's *result* (`return <slot>`).
4. `dk_perform(tag, <arg-slot>, k)` and the handler case's `intptr_t arg`.
5. `dk_invoke((DK*)k, <v-slot>)` (resume value) and its delivered result.
6. The direct->cps entry seed and the final unwrap in the entry trampoline.

Nothing else needs to change representation. In particular, the **captured env
struct** the deferred capture story will use carries real-typed fields, so
non-scalar *captured* values never touch the slot -- this plan and the capture
extension compose cleanly.

Internally, the fix is: **declare binders/atoms/params with their real C type**
(`emit_type_c_name`, which the signature path already uses for params) and
insert a slot cast *only* at the six boundaries above.

## Two emitter primitives

Introduce a pair of helpers in `emit_cps_ir.c`, keyed by the value's
`TypeKind` (or full `Type`):

- `slot_store(ty, expr)` -> a C expression producing the `intptr_t` slot from a
  real-typed value.
- `slot_load(ty, slot_expr)` -> a C expression producing the real-typed value
  from an `intptr_t` slot.

Their bodies are dictated by the value's **representation tier**:

### Tier A -- pointer / <=64-bit-integer sized (fits the slot by cast)

`TY_BOOL`, `TY_INT8..64`, `TY_UINT8..64`, `TY_CSTR` (`const char *`),
`TY_PTR_VOID` / `TY_REF` / `TY_LREF` / `TY_RC` / `TY_WEAK` (pointers), `TY_FN`
(function pointer / boxed `void *`), and **carrier-ABI structs/ADTs**
(`type_uses_carrier_abi`, `emit_core.c:341` -- parametric `Option` / `Result` /
`Pair` etc. ride an `int64` carrier, i.e. a boxed pointer).

- `slot_store(ty, e)` = `(intptr_t)(e)`
- `slot_load(ty, s)`  = `(<ctype>)(s)`  (with the narrow-int zero/sign-extend
  falling out of the cast)

Zero-cost. This tier is the bulk of the remaining blockers, because
carrier-ABI ADTs (Option/Result/map) are already `int64` at runtime -- the
backend rejects them only by `TypeKind`, not by real incompatibility.

### Tier B -- float / double (fits the slot by bit-reinterpret)

`TY_FLOAT` / `TY_FLOAT64` (`double`) and `TY_FLOAT32` (`float`). The bit pattern
fits 64 bits but is not an integer, so reinterpret through the proven idiom
already used elsewhere in the emitter (`emit_expr.c:2541`):

- `slot_store(double, e)` = `((union { double d; int64_t i; }){ .d = (e) }).i`
- `slot_load(double, s)`  = `((union { double d; int64_t i; }){ .i = (s) }).d`
- `float32` reinterprets through a `uint32_t` in the low half of the slot.

Emit these as a tiny static inline pair in the prelude (or inline the union
expression) so the same idiom is not open-coded per site.

### Tier C -- wide by-value aggregates (does not fit; box at the boundary)

Non-parametric flat-product ADTs flow **by value** as a `tur_adt_<Name>`
aggregate (`adt_is_byvalue_product`, `types.h:400`) that can exceed one word.
These cannot ride the slot directly. Two options, decided in N3:

1. **Box at the DK boundary.** `slot_store` heap-copies the aggregate and stores
   the pointer; `slot_load` derefs (and, per ownership discipline, frees on the
   consuming side). Keeps the slot one word at the cost of an allocation per
   crossing -- acceptable given the DK nodes already allocate, and bounded to
   values that actually cross a continuation boundary.
2. **Restrict to boxed representation on the CPS path.** Emit these ADTs through
   their carrier/boxed form when they appear in a colored function, so they are
   Tier A there.

Tier C is the smallest, hardest slice and is gated last; until it lands, a
colored function that threads a wide by-value aggregate *through* a continuation
keeps the whole-function fallback.

## Scope and guiding constraint

Same discipline as the parent plan: **fallback-guarded, incremental,
zero-regression.** Each phase widens `tk_scalar` / `atom_ok` / `fn_sig_ok` from
"scalar" to "slot-representable at tier <= N", inserts the slot casts, and adds
round-trip fixtures asserting **direct-vs-CPS value equality**. Anything above
the current tier keeps falling back; `--dump-cps` remains the diagnostic.

## Phases

### N0 -- slot-crossing convention (de-risk)

Ratify the `slot_store` / `slot_load` convention before touching the emitter, in
the C0 tradition: a throwaway hand-written C sketch under
`tests/probes/cps-abi-c0/` that threads a `const char *`, a `double`, and a
carrier `int64` through `dk_run` / `dk_frame` / `dk_invoke` / `dk_perform` using
the tier casts, compiled and run (and clean under ASan/UBSan). Append the
ratified tier table to this doc.

### N1 -- real binder types + Tier A

- Replace the uniform `int64_t` binder declaration in `emit_binder_decls` with
  `emit_type_c_name(ctx, emit_type_from_kind(binder.ty))` (guarded: a binder
  whose type is not yet slot-representable keeps the function on the fallback
  path via classification, so this never emits an untyped binder).
- Add `slot_store` / `slot_load` and apply them at the six boundaries (Tier A =
  plain casts).
- Widen `tk_scalar` -> `slot_representable_tierA` (Tier A set above). Update
  `atom_ok`, `fn_sig_ok`, and the perform/handle/shift subset predicates in
  lockstep.
- Strings: add a `CA_STR` atom (carrying the `StrSlice`), teach `is_atomic` /
  `atom_of` to produce it, `atom_str` to emit `atom_cstr`, and add
  `BS_PRINTLN_CSTR` to the supported builtin shapes.
- **Round-trip fixtures.** A colored function returning / threading a `cstr`
  (e.g. an effect whose handler returns a string, printed via a `cstr`
  continuation); a colored function over a `ref` / narrow int; a colored
  function over an `Option` (carrier ADT) constructed and matched on the scalar
  path -- each direct-vs-CPS equal.

### N2 -- Tier B floats

- Add the reinterpret `slot_store` / `slot_load` for `double` / `float`.
- Add `BS_PRINTLN_FLOAT` / `BS_PRINTLN_FLOAT32` and the float arithmetic
  builtin shapes to the supported set; declare float binders `double` / `float`.
- **Round-trip fixtures.** Per the STRICT float rule (CLAUDE.md), the first
  probe uses a non-zero fractional literal (e.g. `7.1`, `3.25`), never `7.0`:
  a colored function that performs an effect returning a `float` and does
  `float` arithmetic on the resumed value, asserting the CPS result equals the
  direct-style result to the last bit.

### N3 -- struct / ADT forms

- Translate `EX_MAKE_STRUCT` / `EX_GET_FIELD` / `EX_DEFAULT_OF` in `cps_ir.c`
  (new `CT_*` nodes or `CT_LETPRIM`-shaped lowerings) and emit them in
  `emit_cps_ir.c` for **carrier-ABI** (Tier A) types first -- these are the
  ~1800 stdlib option/result/map occurrences and are pointer-sized, so they need
  translation + emission but no new slot machinery.
- Then decide Tier C (box-at-boundary vs boxed-representation) for wide by-value
  aggregates and implement the chosen path behind the classification guard.
- **Round-trip fixtures.** A colored function constructing and destructuring an
  `Option` / `Result` / a small record across an effect boundary.

### N4 -- non-scalar continuation payloads

Make the six DK boundaries carry the wider values end to end:

- A non-scalar **effect argument** (`perform (E x)` where `x` is a string /
  float / carrier ADT) -> `slot_store` the arg; the handler case `slot_load`s it.
- A non-scalar **resume value** (`resume k v`) and **reset / handle result**.
- The **entry trampoline** returns the real type via `slot_load(retty, __r)`
  instead of the raw `int64_t`.
- Fixtures: an effect that produces and resumes with each tier's value.

### N5 -- corpus re-measure, fixtures, docs

- Re-run the C5 measurement (annotated `<unsupported>` tally) over the effect +
  delimited corpus; confirm the ADT/struct/string blockers are closed for
  colored functions on the critical path and record what tier-C shapes remain.
- Promote a real corpus program (an effect handler over `Option` / `Result`)
  from fallback to CPS-emitted as an end-to-end fixture.
- Fold the non-scalar rules into the `docs/guides/` ABI note (a C6 deliverable),
  documenting the tier table and the slot convention.

## Depends on / reuses

- **Type -> C representation.** `type_c_name` / `emit_type_c_name`
  (`types.c`, `emit_core.c`); `emit_type_from_kind`.
- **Representation classification.** `type_uses_carrier_abi` (`emit_core.c:341`)
  for Tier A carrier ADTs; `adt_is_byvalue_product` (`types.h:400`) for Tier C.
- **Float reinterpret idiom.** The `union { double d; int64_t i; }` pattern
  already emitted at `emit_expr.c:2541`.
- **String atoms.** `atom_cstr(StrSlice)` (`emit_core.c`).
- **The existing backend.** All of `emit_cps_ir.c`'s helper/lifting machinery;
  the six DK boundaries are already the only sites that touch the slot.

## Out of scope

- **Changing the DK slot width.** The slot stays one `intptr_t`; wide values are
  boxed, never widened in the machine.
- **The stackless effect trampoline.** Deep perform/resume recursion is a
  separate concern (parent plan C6 / F-series), orthogonal to value width.
- **Env capture.** Lifting a continuation that captures enclosing locals is a
  separate deferred item; non-scalar captured values simply get real-typed env
  struct fields when that lands (they never cross the slot).
- **Full grammar coverage.** As with the parent plan, the whole-function
  fallback covers any representation not yet tiered, indefinitely.
