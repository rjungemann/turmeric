# Plan: Unboxing and Monomorphization -- Remaining Work

> **Status:** Draft Plan
> **Last Updated:** 2026-05-27
> **Type:** Compiler / Codegen / Type System
> **Related:**
> - `docs/aggregate-carrier-abi-plan.md` (carrier <-> concrete bridge)
> - `docs/guides/type-erasure-guide.md` (current erasure map)
> - `docs/known-bugs.md` (KB-004, KB-008, KB-010, KB-012, KB-015)

---

## Overview

Three related-but-distinct lines of work move Turmeric values out of the
universal `int64_t` carrier and toward real C representations:

1. **Sized primitive types** -- `:int8`/`i32`/`u64`/`float32`/... declared,
   lowered, mangled. **Mostly landed; gaps remain.**
2. **Unboxed structs** -- concrete `defstruct` and fully-applied `TY_APP`
   compile to real C structs. **Layout works; ABI bridge across call
   sites is partial.**
3. **Monomorphization** -- polymorphic globals are cloned per concrete
   instantiation when the C-level ABI changes. **Works for top-level
   defns; does not cover closures, inline-C, or typeclass dispatch.**

This plan inventories what is implemented today, names the gaps, and
proposes phased work to reach a "sensible implementation" -- where
*sensible* means: the language can promise that a fully-typed program
with concrete numeric / struct types emits straight C without
`(int64_t)(intptr_t)` round-trips, and the type erasure boundary is
documented and predictable.

This plan does **not** propose erasure-free generics (specializing every
generic call) -- that is a larger ABI change and is out of scope here.

---

## Current state

### 1. Sized primitive types (Phase N)

**Landed:**

- `TypeKind` enum: `TY_INT8` ... `TY_INT64`, `TY_UINT8` ... `TY_UINT64`,
  `TY_FLOAT32`, `TY_FLOAT64` (`src/compiler/types.h`).
- Name lookup: long and short forms (`int32` / `i32`, `float64` / `f64`)
  in `typekind_from_name` and `typekind_from_symbol`
  (`src/compiler/types.c:2245-2266`, `src/compiler/elab_core.c:39-58`).
- C lowering: `type_c_name()` returns the matching `<stdint.h>` typename
  for each kind (`src/compiler/types.c:1568-1577`).
- Literal emission: `INT8_C`/`INT16_C`/.../`f`-suffix in
  `src/compiler/emit_core.c:657-692`.
- Mangling: each kind contributes its own component to struct-app
  mangle names (`src/compiler/types.c:328-337`) -- KB-013's fix.
- Struct fields: `struct_field_c_type` handles all widths
  (`src/compiler/types.c:488-497`); `defstruct` parses them
  (`src/compiler/elab_structs.c:40-87`, `1317-1325`, `1498-1508`,
  `1559-1567`).
- `(as TargetType expr)` cast lowering (fixtures
  `numeric-types-cast`, `numeric-types`, `numeric-types-struct`).
- Codegen layout: `type_has_concrete_codegen_layout` treats every
  sized kind as concrete (`src/compiler/types.c:273-282`), so they
  participate in struct-app monomorphization correctly.

**Phantom-typed sized data structures (a separate axis):**

- `stdlib/sized.tur` (SizedVec, Size, StaticInt), `stdlib/sized-buf.tur`,
  `stdlib/sized-matrix.tur`, `stdlib/sized-bits.tur` -- all SZ0..SZ3
  phases complete. These track *quantity* in the type system, not C
  width, and live above the codegen layer.

**Gaps:**

- **Mixed-width arithmetic.** Operators like `+`, `<`, `=` between, say,
  `:int16` and `:int32` are not specified. Today everything implicitly
  promotes to `:int` (the canonical 64-bit kind) or fails kind-check;
  there is no documented rule. Decide: implicit narrowing/widening, or
  require explicit `(as ...)`?
- **Overflow / saturation semantics.** Wrapping is the C default for
  unsigned; signed overflow is UB. No language statement on which is
  promised.
- **Bitwise ops on unsigned narrow types.** `bit-and`/`bit-or`/`bit-xor`
  are typed `(:int :int -> :int)` in stdlib today; calling them on
  `:uint16` round-trips through `:int`.
- **Hash / `=` consistency.** A `:int8` value `-1` (`0xFF`) and a
  `:int` value `255` compare equal under value coercion but produce
  different bit patterns. `hash` on the two narrow kinds needs a rule.
- **Inline-C interop.** `extern-c` and `#{Unsafe}` blocks declare
  fixed C signatures; passing a `:int32` argument into an inline-C
  block that reads it as `int64_t` is silently wrong. Need a
  per-parameter width check at the inline-C boundary.
- **Doc story.** There is no user-facing guide to the sized primitives;
  the test fixtures and KB notes are the only references. A
  `docs/guides/sized-primitives-guide.md` would close that.

### 2. Unboxed structs

**Landed:**

- `type_has_concrete_codegen_layout()` returns true for `TY_STRUCT`
  with a non-opaque, non-parametric definition, and for `TY_APP`
  whose head is a concrete struct and whose args are all
  concrete-layout types (`src/compiler/types.c:266-319`).
- `register_struct_app()` mangles a unique name (e.g.
  `Vec__int`, `Result__int__int`) and registers a typedef
  (`src/compiler/types.c:407-429`).
- `type_codegen_emit_struct_apps` emits the concrete C typedef and
  field layout (`src/compiler/emit_module.c:3592-3614`).
- ADT-app monomorphization: TS4P1 walks all expressions and registers
  concrete `(Maybe int)`-style applications
  (`src/compiler/emit_module.c:429-622`).
- `make-struct` produces concrete struct literals when the result type
  is fully concrete (KB-014 fix).

**Gaps:**

- **The carrier <-> concrete bridge is not landed.** Four known sites
  still misroute: `::` ascription (KB-004), `let`-binding annotation
  on a carrier-returning function (KB-010), parametric typeclass
  instance bodies (KB-012), specialization-cache reuse (KB-015).
  `docs/aggregate-carrier-abi-plan.md` covers this; it is a draft.
- **Pass-by-value vs pass-by-pointer policy.** Unboxed structs are
  passed by value at the C level. Anything wider than ~16 bytes (most
  ADTs, `SizedMatrix`, etc.) pays a copy at every call. No policy yet
  for "pass `const T *` when sizeof(T) > N".
- **Generic-struct fields containing `TY_FN`.** `struct_field_c_type`
  hard-codes `int64_t` for `TY_FN` fields
  (`src/compiler/types.c:473`). A fully-instantiated functor-of-int
  struct still carries a generic `int64_t __fn` slot rather than a
  typed function pointer. This blocks devirtualization for closures
  stored in structs.
- **Field-level unboxing of nested structs.** A `struct { Vec2 pos; }`
  embeds the `Vec2` flat today (works), but a `struct { Option[int]
  o; }` does not -- `Option[int]` is a heap pointer in the carrier
  even after the recent fixes. Confirm and decide whether to inline.
- **Inline-C reads of unboxed locals.** Phase 3 of the carrier-bridge
  plan flagged this risk; needs an audit of `stdlib/vec.tur` and
  `stdlib/hamt.tur` after concrete-typed let-bindings land.

### 3. Monomorphization

**Landed:**

- `EmitAbiSpecialization` clones a global function per distinct
  `(arg_types, result_type)` tuple when `type_c_name` of any
  parameter or result differs between the generic signature and the
  concrete instantiation (`src/compiler/emit_module.c:260-345`).
- Driven by `AbiTypeBinding`s that elab attaches to each call when a
  named tyvar substitution is in play
  (`src/compiler/elab_call.c:1918-1926`).
- Clones get mangled names (`emit_abi_clone_name`) and are emitted as
  separate C functions (`emit_module.c:1145-1150`).
- Specialization-call cache keyed by the call `Expr*` -- the second
  callee at the same site reuses the clone (`emit_module.c:239-253`,
  `emit_core.c:492-510`).
- ADT-app variants are pre-registered up front so the constructors
  exist as concrete typedefs (TS4P1 in `emit_module.c:429-622`).

**Gaps:**

- **Closures are not specialized.** `emit_abi_record_specialized_call`
  bails when the binding is a closure
  (`fn_binding->closure_fn_binding`) or non-global. A closure that
  captures one int and returns the same int across instantiations
  stays in the `tur_poly_fn_t` polymorphic wrapper.
- **Inline-C bodies are not specialized.** `fd->body->kind ==
  EX_INLINE_C` early-exits. Stdlib helpers written in inline-C
  therefore never benefit from monomorphization; they keep the
  carrier ABI even when the call site is fully concrete.
- **Typeclass dispatch stays dictionary-based.** S1 of the HKT plan
  chose dictionary passing; per-instance method bodies are not
  cloned at concrete use sites. KB-012's underlying convention
  mismatch is the visible symptom.
- **The "ABI change" test is name-based.** `abi_changes` triggers
  when `strcmp(type_c_name(generic), type_c_name(concrete)) != 0`.
  This catches `int64_t -> int32_t` but not, e.g., generic vs
  concrete struct of the same name -- in practice fine, but
  brittle.
- **No cross-module specialization cache.** Two modules that both
  call `result-map` at type `(int -> int)` each emit their own
  clone. The carrier-ABI form is shared; the specialized form is not.
- **`TY_REC` (Fix-style) stays opaque even when fully concrete.**
  `type_c_name` returns `int64_t` for `TY_REC`
  (`types.c:1645-1646`); recursive types are not monomorphized
  even when their parameter is known. Acceptable for v1, but
  worth tracking.
- **Specialization is "best-effort," silent on failure.** When
  `emit_abi_record_specialized_call` cannot clone (closure /
  inline-C / non-global), the call quietly falls back to the
  carrier path. There is no `-Wabi-fallback` or equivalent
  for benchmarking which calls actually got specialized.

---

## Proposed phases

Phases are independent unless noted. Each phase has a concrete green/red
signal and either lands a fixture set or unblocks a known bug.

### Phase A -- Land the aggregate carrier bridge

This is `docs/aggregate-carrier-abi-plan.md` Phases 1-6. It is the
prerequisite for further unboxing work and clears KB-004, KB-010,
KB-012, KB-015. Quote it as-is; no new design in this plan.

**Signal:** the four fixtures listed in that plan's Phase 6 pass under
both `run.sh` and `run-turi.sh`.

### Phase B -- Sized primitives: mixed-width arithmetic story

1. Decide and document the coercion rule for arithmetic / comparison
   between distinct numeric kinds. Recommended: *no implicit
   coercion*; require `(as ...)`. Rationale: matches the existing
   sized-types feedback (KB-013 elevated sized kinds to first-class);
   implicit widening hides the ABI change.
2. Implement the rule in `elab_core.c`'s numeric-binop type check.
   Today it implicitly normalizes through `TY_INT`; tighten it to
   require equal kinds, emit `TUR-E0042` (new) when violated.
3. Provide overloaded literals for each width (`42i8` etc.) -- already
   landed via the lexer.
4. Add a `docs/guides/sized-primitives-guide.md` that names the
   coercion rule, lists the cast forms, and points at
   `numeric-types-cast` for the test surface.

**Signal:** new fixture `tests/fixtures/sized-mixed-arith-error/`
that exercises every disallowed mixed-width op and expects
`TUR-E0042`; the existing `numeric-types*` fixtures keep passing
unchanged.

### Phase C -- Sized primitives: bitwise + hash + inline-C interop

1. Extend `bit-and`/`bit-or`/`bit-xor`/`bit-shl`/`bit-shr` to be kind-
   preserving: an op on two `:uint16`s returns `:uint16` without a
   round-trip through `:int`. Implement in `elab_core.c` op dispatch.
2. Specify hash equality across widths: a `:int8` of value `n` hashes
   identically to a `:int` of value `n`, but a `:uint8` of value
   `0xFF` does **not** hash equal to `:int -1`. Document and
   implement in `stdlib/hash.tur`.
3. Inline-C boundary check: in `elab_*` for `EX_INLINE_C`, verify
   that each declared parameter type's `type_c_name` matches the
   inline-C signature's expected C type. Today this is silent. Emit
   a warning when narrow types reach inline-C blocks unannotated.

**Signal:** `tests/fixtures/sized-bitwise-narrow/`,
`tests/fixtures/sized-hash-consistency/`,
`tests/fixtures/errors/sized-inline-c-width-mismatch/` all land.

### Phase D -- Unboxed structs: pass-by-pointer policy

1. Pick a threshold (recommendation: `sizeof(T) > 16`, i.e. two
   pointer slots) above which a struct argument is passed as
   `const T *` at the C level and copied back in callee-allocated
   storage if needed.
2. Plumb the choice into `register_struct_app` -- store
   `pass_by_ptr: bool` on `RegisteredStructApp` and consult it from
   every emit site that lowers a struct argument.
3. Audit `make-struct`, field-access, and the carrier bridge from
   Phase A; each must respect the same decision.

**Signal:** an `(emit-c)` snapshot test for a defstruct >16 bytes
shows `const T *` signatures; the runtime fixture compares output
against the by-value version and matches.

### Phase E -- Unboxed function-pointer fields

1. Stop hard-coding `int64_t` for `TY_FN` struct fields in
   `struct_field_c_type`. When the containing struct is concrete
   (all type-params instantiated) and the function type is fully
   concrete (no `TY_TYVAR`), emit a typed function pointer.
2. Update `make-struct` and field-write sites to cast through the
   function-pointer type rather than `(int64_t)(intptr_t)`.
3. Keep the carrier ABI for the generic case -- gate on the same
   `type_has_concrete_codegen_layout` predicate.

**Signal:** a fixture that stores a closure-free `fn` value in a
struct field and calls it via `(.f s)` shows no `intptr_t` casts in
the emitted C; runtime output unchanged.

### Phase F -- Monomorphize closures

1. Lift the closure / non-global guard in
   `emit_abi_record_specialized_call`. When a closure is called at
   fully concrete types, emit a specialized variant alongside the
   `tur_poly_fn_t` form.
2. Decide capture handling: the env struct already has concrete C
   types for captures (`emit_expr.c:1715`); the *function pointer*
   slot is the only thing that needs specialization.
3. Cache by `(closure_template_id, arg_types, result_type)` rather
   than by call-expr, so multiple call sites share one clone.

**Signal:** a benchmark fixture (existing `benchmarks/` harness)
shows the specialized closure path avoiding the polymorphic wrapper
call -- measured by `(emit-c)` snapshot diff and a wall-clock
improvement.

### Phase G -- Specialize inline-C bodies

1. Today inline-C bodies are skipped because their C signature is
   declared inline. Introduce a per-kind-suffixed inline-C variant:
   the developer writes a template `\`\`\`c \<INT_TYPE\> ... \`\`\`` and
   the emitter substitutes the concrete C type per specialization.
2. Or: keep the existing inline-C convention but generate a thin
   wrapper that casts at the boundary, monomorphized per call.
3. Audit `stdlib/list.tur`, `stdlib/option.tur`, `stdlib/vec.tur`
   for inline-C blocks that would benefit. Concrete candidate:
   `cons-list-sum`-style accumulators on `(List int32)` today
   silently widen to int64.

**Signal:** at least one stdlib inline-C helper opts in to the new
form; an existing fixture's emitted C shows the kind-specific
variant (e.g. `cons_list_sum__int32`).

### Phase H -- Specialize typeclass methods

This is the largest item; only attempt after Phase A is solid.

1. Decide between two paths:
   - **(a) Per-instance monomorphization at every concrete use
     site** -- removes the dictionary indirection but multiplies
     emitted code.
   - **(b) Keep dictionaries; specialize the *call* through the
     dictionary** -- emit a direct call when the receiver type is
     statically known, fall back to dict dispatch otherwise.
2. Recommended: start with (b), since it composes with the
   existing dictionary infrastructure and the existing
   `EmitAbiSpecialization` machinery already keys on call-site
   types.
3. Resolve KB-012's convention question once and for all: the
   instance body matches the specialized convention.

**Signal:** a parametric `Eq` instance on `Tuple2[int int]` shows
no `tur_typeclass_dispatch__Eq__eq?` call in the emitted C for a
statically-known receiver; KB-012 fixture passes.

### Phase I -- Specialization observability

A small, cheap, high-leverage phase: add an `--emit-abi-trace` flag
that prints, per call site, which path was taken (concrete-clone,
carrier, dictionary, polymorphic-wrapper). Use it to drive Phase F
and Phase H without flying blind.

**Signal:** `tur emit-c --emit-abi-trace fixture.tur` prints one
line per resolved call; a stdlib benchmark file shows the expected
mix of clones and carriers.

### Phase J -- Cross-module specialization cache (optional)

Once Phases A-H land, two modules importing the same generic def
both emit their own clones. A persistent cache (`.tur-abi-cache/`)
keyed by `(module, fn_name, type_args)` could dedupe.

This is an optimization, not a correctness item. Defer unless
benchmarks justify it.

---

## Out of scope

- **Erasure-free generics** -- specializing every call. The carrier
  ABI exists precisely to avoid that blowup; this plan keeps the
  carrier as the fallback.
- **GC/tagged-pointer reworks** -- the current `int64_t` carrier
  doubles as a GC-invisible handle. A move to tagged pointers
  belongs in a separate runtime plan.
- **SIMD / vector primitives** -- `:int32x4`-style packed types
  are a separate axis. Sized primitives are the prerequisite.
- **Refinement types over sized primitives** (`{x: int32 | x > 0}`)
  -- tracked by `docs/upcoming/refinement-types-plan.md`.
- **Dependent sized types** (lift the phantom `SizedVec` size into
  values) -- noted in the sized-types memory; would build on this
  plan but is not part of it.

---

## Open questions

**All questions resolved (2026-05-27).**

1. **Implicit widening: no.** No implicit coercion between numeric
   widths. Mixed-width arithmetic requires an explicit `(as ...)`.
   Matches the language's "explicit over implicit" stance.
2. **Pass-by-pointer threshold: 16 bytes.** Structs wider than 16
   bytes are passed as `const T *`. No benchmark-driven adjustment
   for now.
3. **Closure specialization cap: none.** Ship Phase F without a
   `--max-specializations-per-fn` cap. Add one only if benchmarks
   show it is needed.
4. **Typeclass path: (b).** Keep dictionaries; specialize the call
   through the dictionary when the receiver type is statically known.
   Fall back to dict dispatch otherwise.
5. **Phase G inline-C: template approach.** The kind-suffixed
   template inline-C form is acceptable for stdlib. Carrier-only
   wrappers are not required.

---

## Verification

Per-phase signals are above. The aggregate signal for the plan is:

- All four KB-004/010/012/015 fixtures pass.
- `numeric-types*` fixtures still pass; new `sized-*` fixtures land.
- An `--emit-abi-trace` run on a representative project shows a
  reasonable mix of concrete clones vs carriers, with carriers
  appearing only at genuinely polymorphic call sites (printf-style
  variadics, type-erased containers, dict-dispatched typeclass
  methods).
- `docs/guides/type-erasure-guide.md` gets a "Status: post-Phase X"
  note and an updated map of which kinds still erase.
