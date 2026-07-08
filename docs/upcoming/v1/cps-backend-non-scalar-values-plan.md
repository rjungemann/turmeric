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

**Status -- started (strings + <=64-bit-int slice landed).** `emit_cps_ir.c`
now declares each binder with its real C type (`binder_ctype` ->
`emit_type_c_name`); `slot_ty` was widened from `{int,bool,int64}` to the Tier A
set (all `<=64`-bit int/uint widths + `bool` + `cstr`); the entry wrapper returns
the real type, and the six DK slot boundaries slot-load the incoming value into a
real-typed local (lifted-frame value param `<x>__slot`, handler-case `arg`,
`resume` result, entry unwrap) -- Tier A `slot_store` stays the existing
`(intptr_t)(...)` cast and `slot_load` is `(<ctype>)(slot)`. Strings landed: a
`CA_STR` atom carrying the `StrSlice`, `is_atomic`/`atom_of` produce it,
`atom_str` emits `atom_cstr`, and `BS_PRINTLN_CSTR` is supported. A `cps->direct`
synchronous call binds its result with `__auto_type` so a non-scalar-returning
callee keeps its real type. Round-trip fixture `tests/fixtures/cps-backend-cstr/`
threads a `cstr` through perform/handle/resume (result `world`); the widening
also lets stdlib functions with `cstr` params CPS-emit.

**Update -- narrow int + raw pointer.** `slot_ty` also admits `TY_PTR_VOID`
(`ptr<void>`), the last Copy, non-owning pointer shape -- a raw pointer casts
through the slot exactly like a `cstr`, so bit-threading it is unambiguously
correct. Two more round-trip fixtures landed:
`tests/fixtures/cps-backend-narrow-int/` threads an `int32` through
perform/handle/resume (the lifted handler frames slot-load `int32_t t0 =
(int32_t)t0__slot` and the resume result as `int32`; result `41`), and
`tests/fixtures/cps-backend-ptr/` threads a `ptr<void>` through an effect that
hands back a raw buffer pointer, resumed and derefed on the CPS path (the
perform-cont slot-loads `void * t0 = (void *)t0__slot`; result `7`).

**Update -- narrow-int arithmetic + the 64-bit reinterpret.** Narrow-int *math*
also works on the CPS path: the arithmetic builtin shapes (`BS_BIN_INFIX`,
`BS_VARIADIC_FOLD`, ...) already apply to the sub-64-bit widths, so a colored
function that computes `(+ (perform ...) (:: 1 :int32))` stays CPS (the
`cps-backend-narrow-int` fixture now does int32 arithmetic; result `42`). Two
supporting fixes: `atom_ok`/`atom_str` accept and emit any `<=64`-bit integer
literal (right `*_C()` suffix), and -- the substantive one -- `cps_ir.c` now
peels a **same-size Tier A `EX_REINTERPRET`** (`is_tierA_reinterp` /
`tierA_scalar_kind`). The frontend lowers `(:: N :uint64)` / `(:: N :int64)` as
an `int -> {u}int64` reinterpret (both 64 bits, so bit-identical rather than a
widening cast); without peeling it, a colored function using a 64-bit ascribed
literal fell back. New `cps-backend-uint64` fixture threads a `uint64` through
perform/handle/resume with arithmetic (result `42`). Full suite: 1995 passed,
0 failed (before these fixtures; +1 with `cps-backend-uint64`).

**Correctness note (resolved) -- perform/handle co-classification.** Fixing the
reinterpret exposed a **general** hole: the backend classified each colored
function for CPS emission independently, but a `perform` and its `handle` must be
on the *same* machine (both DK, or both fiber). When the classifier admitted a
performer to the CPS set while its handler fell back (or vice versa),
`dk_perform` found no DK handler and the program aborted with "unhandled
effect". Resolved by a **co-classification fixpoint** in `ensure_S`: every
colored top-level function enters the table (non-candidates kept with
`in_s=false`); each function's performed/handled effect set is collected by a
**raw-Expr** walk (`expr_collect_effects`) so effects hidden under
non-lowered forms like `match`, and effects in *uncolored* fiber performers, are
still seen; effects reached only by never-CPS code fold into a `base_taint`.
Rule B of the fixpoint evicts every in-S function that touches a tainted effect,
so a performer and its handler are never split. Regression fixtures:
`cps-backend-handler-fallback` (colored handler falls back on a captured param)
and `cps-backend-effect-under-match` (uncolored performer, effect hidden under
`match`, coupled only dynamically). Archived report:
`docs/archive/cps-backend-perform-handle-machine-split.md`.

Still open in N1: owning pointer types (`ref`/`rc`/`weak`/`lref`) -- but **not**
as a `slot_ty` addition. The owning-pointers investigation
([cps-backend-owning-pointers-plan.md](cps-backend-owning-pointers-plan.md))
found these are never bare slot values (a bare owning pointer cannot be a
function result / continuation payload -- it is a source-level type error), so
they cross a continuation only inside structs / ADTs and their discipline folds
into carrier-ABI ADTs (N3) plus one CPS-specific remainder: drop-node
translation (O1). `slot_ty` deliberately omits them (and the non-owning borrows,
which would outlive their referent). Also still open: carrier-ABI ADTs (need
full-`Type` info the IR does not yet carry -- folds into N3). The
`slot_store`/`slot_load` seams are in place for Tier B/C to extend.

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

### N6 -- remove the whole-function fallback (graduation prerequisite)

Once N0-N5 leave no value representation a colored function can name still on
the fallback path, delete the fallback itself:

- Remove the `CT_UNSUPPORTED` whole-function bail-out and the direct-vs-CPS dual
  path for colored functions from `emit_cps_ir.c` / the classifier
  (`ensure_S`, `term_core_ok`, `slot_ty` / `atom_ok` / `fn_sig_ok` and the
  perform/handle/shift subset predicates). A colored function is emitted through
  the CPS backend, full stop -- there is no second lowering to fall back to.
- Any form that still cannot be emitted becomes a hard compiler error at that
  point, not a silent reroute to the fiber path. Reaching N6 means there are no
  such forms for colored code; if one is discovered, it is a blocker to fix, not
  a fallback to tolerate.
- Re-run the full suite and the sign-off probe with the fallback gone; the CPS
  backend is now the sole lowering for colored functions.

This phase is what the graduation gate below turns on.

## Graduation gate -- what must hold before `cps-backend` goes always-on

**Policy (owner decision, supersedes the parent plan's "fallback covers the rest
indefinitely" stance).** The `cps-backend` experiment does **not** graduate --
does not go always-on and shed its `--enable` gate -- until every value
representation a colored function can name is emitted natively **and the
whole-function fallback is removed** (N6). The fallback is temporary scaffolding
for the incremental rollout, not a permanent escape hatch. All of the following
must be true at graduation:

1. **Tier A complete** (N1) -- every `<=64`-bit integer/bool width, `cstr`, and
   raw `ptr<void>` thread natively. *Done.*
2. **Tier B complete** (N2) -- `float` / `double` / `float32` thread via the
   bit-reinterpret slot convention.
3. **Tier C complete** (N3) -- wide by-value aggregates cross the boundary by the
   chosen boxing strategy.
4. **Owning pointers handled correctly on the CPS path** -- `ref` / `rc` /
   `weak` / `lref`. Investigation
   ([cps-backend-owning-pointers-plan.md](cps-backend-owning-pointers-plan.md))
   found these are **never bare slot values** (a bare owning pointer cannot be a
   function result / continuation payload in the source language -- it is a type
   error), so there is **no `slot_ty` row to add**. They cross a continuation
   only as fields of a struct / ADT, so their discipline is the enclosing
   aggregate's drop glue and folds into item 6 / N3 (O2 there). The one
   CPS-specific remainder is owning-value locals (O1) -- **landed for explicit
   `rc` ops** (`rc/of` / `rc/clone` / `rc/drop` / `rc/strong-count` / `rc->ptr`)
   with atomic operands: a new `CT_LETRAW` IR node delegates their emission to
   the direct emitter (`emit_value`), so a colored function with a local `rc`
   CPS-emits with its drop run exactly once (fixture `cps-backend-rc-drop`,
   LeakSanitizer-clean). The owning value stays a local and never crosses the
   slot; the zero-capture cut holds off the multi-shot double-free hazard. O1
   tail still open: `EX_DEFER` auto-drops (`ref<T>`) and non-atomic operands.
   **Current state is safe: any owning-pointer case not yet handled falls back,
   and the fallback handles ownership correctly (LeakSanitizer-clean).**
5. **Narrow-int arithmetic shapes** -- `+`, `-`, `*`, comparisons, etc. on the
   sub-64-bit integer widths are in the supported builtin-shape set, so a colored
   function that does narrow-int *math* (not just threads a narrow int) stays on
   the CPS path. *Done* (the arithmetic shapes already covered the narrow widths;
   the `cps-backend-narrow-int` / `cps-backend-uint64` fixtures exercise int32 /
   uint64 math end to end).
6. **Carrier-ABI ADT forms** (N3) -- `EX_MAKE_STRUCT` / `EX_GET_FIELD` /
   `EX_DEFAULT_OF` for carrier-ABI Option/Result/map and friends are translated
   and emitted, closing the ~1800 stdlib occurrences C5 measured.
7. **Fallback removed** (N6) -- no `CT_UNSUPPORTED` whole-function bail-out
   remains for colored functions; the direct-vs-CPS dual path is retired. The
   **perform/handle machine-split** hole
   (`docs/archive/cps-backend-perform-handle-machine-split.md`) is already closed
   by the co-classification fixpoint in `ensure_S`, so while both paths coexist
   no perform/handle pair is split; N6 then removes the fallback entirely.

Until all seven hold, the experiment stays gated. The C6 sign-off in the parent
plan (`docs/upcoming/v1/cps-ir-to-c-backend-plan.md`) must not mark
`cps-backend` graduated -- and the release-cut skills must not bump past its
`expires_at` -- while any item above is open.

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
- **Full grammar coverage *during rollout*.** While N0-N5 are in flight the
  whole-function fallback covers any representation not yet tiered. This is
  explicitly **not** permanent: per the graduation gate above, the fallback is
  removed (N6) before `cps-backend` graduates. (This reverses the parent plan's
  original "covers the rest indefinitely" wording, which that plan's own
  out-of-scope note is updated to match.)
