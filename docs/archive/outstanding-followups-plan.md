# Plan: Outstanding follow-ups across five archived plans

> **Status:** In Progress
> **Last Updated:** 2026-05-30
> **Type:** Compiler / Codegen / Type system / Tooling -- roadmap
> **Consolidates outstanding work from:**
> - [`docs/archive/history/arbitrary-arity-kinds-plan.md`](archive/history/arbitrary-arity-kinds-plan.md)
> - [`docs/archive/history/codegen-clang-int-pointer-cleanup-plan.md`](archive/history/codegen-clang-int-pointer-cleanup-plan.md)
> - [`docs/archive/history/cross-module-specialization-cache-plan.md`](archive/history/cross-module-specialization-cache-plan.md)
> - [`docs/archive/history/tur-run-plan.md`](archive/history/tur-run-plan.md) -- basic `tur run` impl landed; `tur new` + extended phases outstanding
> - [`docs/archive/history/unboxing-and-monomorphization-plan.md`](archive/history/unboxing-and-monomorphization-plan.md) -- Phases G/H/I landed; A/B/C/D/E/F outstanding

---

## Progress tracker

Re-verified against HEAD before execution (per "How to read this plan"). Most
themes had already landed; the items below capture the genuinely outstanding
work and its disposition.

| Theme | State | Notes |
| --- | --- | --- |
| A -- Clean Clang codegen | **DONE (verified)** | All 5 canary fixtures + all 73 snapshots compile clean under `clang -Werror=int-conversion -Werror=incompatible-function-pointer-types`; no `-Wno-error` downgrade in `tests/run.sh` or compiler defaults. |
| B -- Aggregate carrier bridge | **DONE (verified)** | 4 gate fixtures under `tests/fixtures/typed-slots/` pass; KB-004/010/012/015 fixed. |
| C -- Sized primitives polish | **DONE (verified)** | C1 (`TUR-E0042`), C2 (kind-preserving bitwise), C3 (hash consistency), C4 (`TUR-W0037`) all implemented with fixtures + guide. |
| D -- Unboxed struct ABI | **DONE** | D1 (`pass_by_ptr` >16B) + D2 (typed fn-ptr fields) already landed; D3 (nested-aggregate decision: keep carrier-erased, documented in type-erasure-guide.md) + D4 (inline-C unboxed-local audit: vec.tur/hamt.tur are clean, all carrier-ABI) completed here. |
| E -- Arbitrary-arity kinds | **DONE** | E1-E4 already landed; E5 completed here -- added `tur_kind_arity_unit` C unit test (round-trip through arity 15 + >15 boundary); docs already current. |
| F -- Monomorphize closures | **PARTIAL** | The landed "Phase F" concrete-dispatch fast path (`is_poly_call` branch in `emit_expr.c`) was extended here to the unsigned narrow ints (`TY_UINT8/16/32`) alongside the signed siblings, so narrow-int closure/poly applications skip the `int64_t` carrier round-trip (fixture `phase-f-poly-concrete-unsigned/`). The clone-generation F1/F2/F3 (standalone specialized closure variants) still bails in `emit_module.c` and stays deferred as deep codegen work -- see the Theme F status notes below. |
| G -- Cross-module specialization | **IN PROGRESS** | J1-J5, J7 DONE (correctness landed). J6: `--no-abi-cache` / `TUR_NO_ABI_CACHE` added here (suppresses the `.tur-abi-cache/` machinery). The cache *read* + ownership-reuse + invalidation -- a pure rebuild-time optimization -- remains outstanding (cold cache already builds correctly). |
| H -- Tooling (`tur run` / `tur new`) | **IN PROGRESS** | Most RN*/NW* done. NW0 (length + reserved-name validation) completed here, plus a `tur check <dir>` directory-mode fix (was crashing on a directory arg). NW6 (bootstrap CI gate) is **blocked on larger work** -- see note below. |

---

## How to read this plan

The five source plans were drafted independently, and several have had work
land on `main` after they were archived (visible artefacts: `.tur-abi-cache/`
in `src/main.c`, `emit_abi_scan_program` in `emit_module.c`, `tur run --init`
in `src/compiler/justrun.c`, the `external_linkage` field on
`EmitAbiSpecialization`). This file does **not** reverify each archived
phase line-by-line; before starting any theme below, grep the named symbols
in current HEAD and trim items already done.

Themes are ordered for **dependency** first, **risk** second:

1. Theme A -- Clang correctness blocks standalone macOS builds. Land first.
2. Theme B -- Aggregate carrier bridge unblocks Themes C/D/E.
3. Themes C--D -- Sized primitives polish; unboxed-struct ABI completion.
4. Theme E -- Lift arity-5 cap. Independent of the codegen work above.
5. Theme F -- Monomorphize closures. Builds on Themes B/D.
6. Theme G -- Cross-module specialization. Builds on the whole-program
   specialization pipeline that A--F refine.
7. Theme H -- Tooling polish (`tur run` extended phases, `tur new` spec
   alignment). Independent of the compiler work.

Cross-cutting note: Theme A is a *prerequisite for portable testing* of
B--G on macOS Clang. Land it before doing wide-impact codegen changes that
would otherwise need the same `-Wno-error` downgrade.

---

## Theme A -- Clean Clang codegen (correctness, blocks standalone macOS build)

> Source: `codegen-clang-int-pointer-cleanup-plan.md`. Five fixtures still
> rely on the Clang-gated `-Wno-error=int-conversion
> -Wno-error=incompatible-function-pointer-types` downgrade in
> `tests/run.sh`. A standalone `tur build` / `tur run` on macOS with
> `TUR_CC_FLAGS` unset still hits the errors -- the compiler's own default
> flags in `src/main.c` do not carry the downgrade.

### A1 -- Family A: carrier <-> pointer bridge insertion

Route every carrier <-> pointer transition through the existing
`emit_carrier_bridge` plumbing (KB-021). Extend the "needs a bridge"
predicate (`expr_emits_byvalue_carrier_abi` and callers in
`src/compiler/emit_expr.c`, `src/compiler/emit_module.c`) to cover:

- struct-field initialisation of a carrier-ABI field from a pointer-typed
  value (and vice versa) -- the `exg4-pack-into-struct` and
  `exg4-pack-into-struct-via-let` fixtures;
- `return` of a carrier where the C return type is a pointer (the
  `NULL`/exists case in `exg5-exists-cycle`);
- argument passing where the parameter's C type is a concrete pointer
  (`RcControlBlock *`, `tur_exists_t`, ...) but the argument is a carrier
  (`exg5-rc-in-exists`).

All transitions must emit `(int64_t)(intptr_t)e` or `(T *)(intptr_t)e` rather
than a bare assignment / return / pass.

### A2 -- Family B: dictionary method-slot typedef matches `pass_by_ptr`

When the typeclass dictionary's method-slot function-pointer typedef is
synthesised (`emit_module.c`, dict-struct emission for `definstance`),
consult `StructDef.pass_by_ptr` (already used to decide the method
definition's by-pointer signature) so that a `pass_by_ptr` instance method
gets a `const T *` slot parameter, not by-value `T`. The
`derive-show-nested` fixture is the canary.

Alternative considered in the source plan: emit dictionary method slots
uniformly on the carrier ABI (`int64_t` params) and bridge at the call
site -- matches the parametric path KB-021 took for struct-typed dispatch.
Pick whichever is smaller in diff; both reach the same observable
behaviour.

### A3 -- Remove the downgrade and the standalone-Clang gap

Once A1/A2 land:

- Compile every previously-failing fixture with
  `clang -Werror=int-conversion -Werror=incompatible-function-pointer-types`
  to reproduce the macOS failure on any platform.
- Drop the Clang-gated `-Wno-error=...` block from `tests/run.sh`.
- Confirm `src/main.c`'s default `cc` flags need no special-casing for
  macOS, so `tur build` works on Clang without any `TUR_CC_FLAGS` export.
- Re-check codegen snapshots (`expected.c`) for any fixture whose emitted
  C changes; regenerate as needed.

**Signal:** the full suite is green on both GCC and Clang with the
downgrade removed; standalone `tur build` on macOS succeeds with
`TUR_CC_FLAGS` unset.

---

## Theme B -- Aggregate carrier bridge (Unboxing Phase A)

> Source: `unboxing-and-monomorphization-plan.md` Phase A; quotes
> `docs/aggregate-carrier-abi-plan.md` Phases 1--6 as-is. Prerequisite
> for Themes C/D/E and for KB-004, KB-010, KB-012, KB-015.

Land the aggregate carrier bridge per the existing
`aggregate-carrier-abi-plan` (no new design in this plan). The four
fixtures named in that plan's Phase 6 are the gate.

**Signal:** the four fixtures pass under both `run.sh` and `run-turi.sh`.
KB-004, KB-010, KB-012, KB-015 close.

---

## Theme C -- Sized primitives polish (Unboxing Phases B + C)

> Source: `unboxing-and-monomorphization-plan.md` Phases B and C. The
> sized-types ladder is largely landed (TY_INT8..TY_FLOAT64, type_c_name,
> literal emission, mangling, casts via `(as ...)`). What's missing is
> a coherent *story* across arithmetic, hash, and inline-C.

### C1 -- Mixed-width arithmetic: no implicit coercion

Resolved in source plan as "no implicit widening" (open question #1).
Implement:

- Tighten the numeric-binop type check in `elab_core.c` -- today it
  implicitly normalises through `TY_INT`. Require equal kinds; emit
  `TUR-E0042` (new) for the mismatch.
- Confirm overloaded width-suffixed literals (`42i8`, etc.) still produce
  their declared kind.
- Add `docs/guides/sized-primitives-guide.md` naming the rule and listing
  the cast forms; point at `numeric-types-cast` for the test surface.

**Signal:** `tests/fixtures/sized-mixed-arith-error/` exercises every
disallowed mixed-width op and expects `TUR-E0042`; existing
`numeric-types*` fixtures keep passing unchanged.

### C2 -- Bitwise ops are kind-preserving

`bit-and` / `bit-or` / `bit-xor` / `bit-shl` / `bit-shr` are typed
`(:int :int -> :int)` in stdlib today; an op on two `:uint16`s
round-trips through `:int`. Make these kind-preserving in `elab_core.c`
op dispatch -- the result kind equals the (now-equal) input kinds.

### C3 -- Hash / `=` consistency across widths

Spec: a `:int8` of value `n` hashes identically to a `:int` of value `n`;
a `:uint8` of value `0xFF` does *not* hash equal to `:int -1`. Document in
`stdlib/hash.tur` and implement. Fixture:
`tests/fixtures/sized-hash-consistency/`.

### C4 -- Inline-C interop boundary check

In `elab_*` for `EX_INLINE_C`, verify that each declared parameter type's
`type_c_name` matches the inline-C signature's expected C type. Today
this is silent. Emit a warning when a narrow type reaches an inline-C
block unannotated. Fixture: `tests/fixtures/errors/sized-inline-c-width-mismatch/`.

(Note: Phase G's `__TUR_TY_<NAME>__` template marker -- already landed --
gives stdlib helpers a clean way to opt in to width-specific inline-C
without a round-trip.)

---

## Theme D -- Unboxed struct ABI completion (Unboxing Phases D + E)

> Source: `unboxing-and-monomorphization-plan.md` Phases D and E.
> Resolved in source plan: pass-by-pointer threshold = 16 bytes (open
> question #2).

### D1 -- Pass-by-pointer policy for wide structs

Above the 16-byte threshold, a struct argument is passed as `const T *`
at the C level and copied back in callee-allocated storage if needed.

- Store `pass_by_ptr: bool` on `RegisteredStructApp` and `StructDef`
  (the field already exists on `StructDef` per Theme A above).
- Plumb the choice through `register_struct_app` and consult it from
  every emit site that lowers a struct argument.
- Audit `make-struct`, field-access, and the Theme B carrier bridge --
  each must respect the same decision. Where Theme A's Family B re-uses
  the same `pass_by_ptr` field, the decision points must agree.

**Signal:** an `(emit-c)` snapshot test for a defstruct >16 bytes shows
`const T *` signatures; the runtime fixture's output matches the
by-value version byte-for-byte.

### D2 -- Unboxed function-pointer fields

Stop hard-coding `int64_t` for `TY_FN` struct fields in
`struct_field_c_type` (`src/compiler/types.c:473`). When *both* the
containing struct and the function type are fully concrete (no
`TY_TYVAR`), emit a typed function pointer. Update `make-struct` and
field-write sites to cast through the function-pointer type rather than
`(int64_t)(intptr_t)`. Keep the carrier ABI for the generic case --
gate on `type_has_concrete_codegen_layout`.

**Signal:** a fixture that stores a closure-free `fn` value in a struct
field and calls it via `(.f s)` shows no `intptr_t` casts in the emitted
C; runtime output unchanged. Devirtualization for "closures stored in
structs" becomes possible.

### D3 -- Nested-aggregate inlining audit

`struct { Vec2 pos; }` already embeds `Vec2` flat. Confirm whether
`struct { Option[int] o; }` is heap-pointer-shaped in the carrier after
Theme B lands; decide whether to inline. (Source plan flags this as
"confirm and decide.") Document the decision in
`docs/guides/type-erasure-guide.md`.

### D4 -- Inline-C reads of unboxed locals -- audit

Phase 3 of `aggregate-carrier-abi-plan.md` flagged this risk. Audit
`stdlib/vec.tur` and `stdlib/hamt.tur` for inline-C helpers that read a
local declared with a concrete type; any helper that assumes `int64_t`
storage must either keep the carrier ABI or opt in to the Phase G
template marker.

**Audit outcome (2026-05-30): clean, no changes required.** Every inline-C
helper in `stdlib/vec.tur` declares its parameters as `:int` (and reads
the vec struct through `(void*)(intptr_t)v`); none declare a narrow sized
type (`:int8`..`:uint64`/`:float32`), so the `int64_t` storage assumption
is correct by construction -- they already keep the carrier ABI.
`stdlib/hamt.tur` is `extern-c`-driven (one inline-C fence) over
`:ptr<void>` / `:int` / `:cstr` params only, with no narrow-typed locals.
Neither file needs the Phase G `__TUR_TY_<NAME>__` template marker.

---

## Theme E -- Arbitrary-arity kinds (lift Tuple5 / KIND_ARROW5 cap)

> Source: `arbitrary-arity-kinds-plan.md`. Independent of the codegen
> work above. Resolved approach: Option A (`typedef uint16_t Kind;`).

### E1 -- Redefine `Kind` as integer-backed

In `src/compiler/types.h`, replace the `Kind` enum with
`typedef uint16_t Kind;` plus `static const Kind KIND_STAR = 0;` etc.
Encoding: `0 -> KIND_STAR`; `1..N -> KIND_ARROW{N}` (arity = value);
`0xFFFF -> KIND_ROW`. `KIND_ARROW2 / KIND_ARROW3 / ...` remain valid
named constants so the ~170 reference sites do not churn.

### E2 -- Rewrite the two real switches

Only two functions destructure `Kind` today (`grep -rn "case KIND_"
src/compiler/`):

- `kind_to_string` (`types.c:2113`): handle `KIND_ROW`, then build
  `"* -> * -> ... -> *"` in a small `Buf` from `arrows = (uint32_t)k`.
  Memoise the first ~16 strings in a static table so the common case
  remains zero-alloc and the return value can stay `const char *`.
- `kind_apply_one` (`types.c:2152`):
  `return (k == KIND_STAR || k == KIND_ROW) ? k : (Kind)(k - 1);`.

`kind_for_arity` becomes `return (Kind)n;` (no cap). `kind_eq` stays
`a == b`. `kind_parse` (`types.c:2126`) already counts `" -> *"` suffixes
and calls `kind_for_arity`, so it inherits the new behaviour.

### E3 -- `hkt_kind` storage / layout assert

`Type.hkt_kind` (`types.h:374`) stays `Kind` and remains byte-compatible
with the existing layout. Add `static_assert(sizeof(Kind) == 2)` and a
comment noting the new encoding. Rollback path: widen the typedef to
`uint32_t` if a layout surprise appears.

### E4 -- Stdlib: extend the Tuple family

Add `Tuple6`..`Tuple8` (or higher) to `stdlib/tuple.tur` following the
existing template: `defstruct`, `tupleN`, and `tupleN-1st`..`tupleN-Nth`
accessors. No hard cap any more; the ceiling is purely "how many
handwritten accessors do we want to ship." A macro-codegened ladder is
out of scope.

### E5 -- Fixtures + docs

Fixtures:

- `tests/fixtures/tuple-arity-6/` -- construct + destructure a `Tuple6`
  end-to-end.
- `tests/fixtures/kind-arity-roundtrip/` -- `kind_parse` /
  `kind_to_string` round-trip for `arity=8` (a C unit test if that's
  where kind helpers are tested today, otherwise drive from `tur eval`
  via a small intrinsic).
- One test exercising a typeclass declared over a 6-ary constructor to
  confirm `elab_types.c:1573` and `elab_structs.c:673` still produce the
  right `hkt_kind`.

Docs:

- Update the comment block at `types.h:41-53` to describe the new
  encoding.
- Update the comment at `types.h:1107-1115` to remove the "arity-cap"
  language.
- Update the `tur/tuple` module docstring (`stdlib/tuple.tur:1-12`) to
  reflect that the ladder is open-ended.

Each step is independently revertable.

---

## Theme F -- Monomorphize closures (Unboxing Phase F)

> Source: `unboxing-and-monomorphization-plan.md` Phase F. Closures are
> not specialized today: `emit_abi_register_call` (`emit_module.c`)
> bails when the binding is a closure (`fn_binding->closure_fn_binding`)
> or non-global, so a closure that captures one int and returns one int
> across instantiations stays in the `tur_poly_fn_t` wrapper. Resolved
> in source: no per-fn specialization cap (open question #3).

> **Status (2026-05-30): OUTSTANDING -- deferred as deep codegen work.**
> Verified the guard is intact at `src/compiler/emit_module.c:295-296`
> (`fn_binding->closure_fn_binding || !fn_binding->is_global`) with a
> second bail at line 313 (`fd->closure`). No `closure_template_id` exists
> in the tree (F3). Lifting these guards to emit specialized closure
> variants alongside the `tur_poly_fn_t` wrapper -- and re-keying the
> spec cache by `(closure_template_id, arg_types, result_type)` -- is a
> genuine monomorphization feature in the whole-program ABI-specialization
> pipeline, with real miscompilation/link-error risk if done carelessly.
> Theme G (cross-module) is explicitly gated on it. It is left for a
> dedicated change-set with benchmark + `--emit-abi-trace` verification
> rather than attempted speculatively here.

> **Update (2026-05-30, follow-up): a lighter-weight slice of Phase F is
> already in the tree and was extended here.** Separate from the
> clone-generation path guarded in `emit_abi_register_call`, the poly-call
> emitter (`emit_expr.c`, the `is_poly_call` branch) carries a "Phase F"
> *concrete-dispatch* fast path: when a `(forall [a] (-> a a))` value is
> applied at a sub-64-bit integer type, it casts the live `tur_poly_fn_t.fn`
> pointer to the concrete C signature and calls it directly, instead of
> widening the argument/result through the `int64_t` carrier. This avoids
> the carrier round-trip without generating a separate clone, and works for
> both native closure thunks and the generic carrier wrapper (the narrow
> int rides the low bits of an integer register on x86-64 SysV). It was
> gated on `type_kind_is_poly_concrete`, which covered only the *signed*
> narrow ints + bool (`TY_BOOL`/`TY_INT8`/`TY_INT16`/`TY_INT32`). This
> change adds the unsigned siblings (`TY_UINT8`/`TY_UINT16`/`TY_UINT32`),
> which share the identical register/extension behaviour; floats stay
> excluded (the carrier wrapper returns in `rax`, not `xmm0`), and full-width
> `(u)int64` needs no specialization since it already coincides with the
> carrier. Fixture: `tests/fixtures/phase-f-poly-concrete-unsigned/`.
>
> The deep F1/F2/F3 work -- emitting standalone *specialized clones* for
> captured closures (re-keying the spec cache by
> `(closure_template_id, arg_types, result_type)`, rewriting the closure
> call site at `emit_expr.c`'s `closure_fn_binding` branch, and emitting a
> clone body that is not thunk-shaped) -- remains deferred for the reasons
> above. The concrete-dispatch fast path already covers the common
> narrow-int closure-application case without that risk.

### F1 -- Lift the closure / non-global guard

In `emit_abi_record_specialized_call`, allow specialization when a
closure is called at fully concrete types. Emit the specialized variant
alongside the `tur_poly_fn_t` form (do not delete the wrapper -- closure
values still need it).

### F2 -- Capture handling stays as-is

The env struct already has concrete C types for captures
(`emit_expr.c:1715`). The function-pointer slot is the only thing that
needs specialization.

### F3 -- Cache by template id, not by call-expr

Replace the per-call-expr cache key with `(closure_template_id,
arg_types, result_type)` so multiple call sites share one clone.

**Signal:** an `(emit-c)` snapshot diff for a benchmark fixture
(existing `benchmarks/` harness) shows the specialized closure path
avoiding the polymorphic wrapper call; wall-clock improvement on the
matching benchmark.

---

## Theme G -- Cross-module specialization (Unboxing Phase J via cross-module-cache plan)

> Source: `cross-module-specialization-cache-plan.md`. Partial work has
> landed on `main` since the plan was archived (`.tur-abi-cache/`
> writers in `src/main.c:1170, 3159`; `emit_abi_scan_program` in
> `emit_module.c:797`; `external_linkage` flag on the spec struct).
> **Re-verify state before starting each sub-phase.** What follows is
> the *originally scoped* roadmap; cross out any item already on `main`.

### G1 -- Part 1: per-module specialization (correctness-bearing)

- **J1 -- factor the scan/emit out of `emit_program`.** Reusable helpers
  both `emit_program` and `emit_implementation` can call against their
  already-flattened `items`: `emit_abi_scan_program`, clone-forward-decl
  loop, clone-body loop. *Status check:* `emit_abi_scan_program` exists;
  confirm the body-loop has been factored as well.
- **J2 -- run the scan in `emit_implementation`.** After `impl_items` is
  flattened, call `emit_abi_scan_program`, emit clone forward decls into
  the file buffer before the function definitions, and emit clone bodies
  after Pass 1. Wire `--emit-abi-trace` on this path too. Subtlety:
  `emit_abi_find_fn_expr` only finds defs in the current module's
  `items`, so clones are naturally produced only in the *owning* module
  -- caller modules still need the call-site rewrite plus an `extern`
  decl (see J4).
- **J3 -- external linkage for shared clones.** Per-spec
  `bool external_linkage` (default false to preserve whole-program
  behaviour). When set: `emit_abi_forward_decl` drops the `static`
  prefix and emits the decl into the **header** (so importers pick it
  up via `#include`), or into the impl as a plain extern; the clone
  body loop emits a non-`static` definition; `emit_fn_def` honours a
  "force external" signal alongside `fn_name_override`. ODR safety:
  clone names are globally unique by construction.

### G2 -- Part 2: ownership + persistent cache (optimization)

- **J4 -- ownership map + `extern` borrow via the owner's header.**
  Within a single multi-module build, exactly one module owns (defines)
  each clone; the rest borrow (declare `extern`). Have the owning module
  emit the clone's forward decl into its **header**, gated on
  `external_linkage`. Borrowers then need no per-caller extern
  bookkeeping -- only "don't emit the body." J1--J4 alone satisfy the
  spirit of Phase J (no duplicate clone bodies across modules).
- **J5 -- the `.tur-abi-cache/` directory.** *Status check: partial.*
  Persist ownership across build invocations under
  `<build-root>/.tur-abi-cache/`; append to `.gitignore` on first
  creation (mirror `.tur-repl-cache` behaviour). One line per clone,
  tab-separated: `<clone_name>\t<owning_module>\t<source_hash>`. Read
  at start of `cmd_build_multi`; rewrite atomically (temp + rename)
  after a successful build.
- **J6 -- cache consultation during the build.** Thread a shared
  `AbiCacheCtx*` through per-module `compile_to_implementation` calls.
  Per spec a module would own: if the cache has no entry, or names this
  module, or the entry's `source_hash` is stale -> own (set
  `external_linkage`, emit body + header decl, update index). If the
  cache names another live module -> borrow. Conservative invalidation:
  drop an entry whose owner's hash changed or whose owner is no longer
  in the build. Cold/empty cache must always produce a correct build.
  Add a `--no-abi-cache` build flag (and/or `TUR_NO_ABI_CACHE=1`),
  mirroring `TUR_NO_AUTO_SPICE`.

  > **Status (2026-05-30): PARTIAL.** The opt-out is done: `--no-abi-cache`
  > and `TUR_NO_ABI_CACHE=1` (`parse_no_abi_cache` / `g_no_abi_cache` in
  > `src/main.c`) now suppress the `.tur-abi-cache/` write in both
  > `cmd_build_multi` and `cmd_emit_c_to_dir`; the build stays correct with
  > the cache disabled. The remaining work -- reading the index at the
  > start of the build, threading an `AbiCacheCtx*` to reuse cross-build
  > ownership for byte-identical rebuilds, and the conservative
  > invalidation -- is a **pure optimization**: the cache is write-only
  > today, so every invocation is already a correct "cold" build (the J6
  > correctness invariant). It is deferred rather than rushed, since an
  > incorrect read would mis-assign clone ownership (duplicate or missing
  > clone bodies -> link errors). Note the current index format is
  > `clone_name\towning_module\tfn_symbol\tresult_kind\tn_args\targ_kinds...`
  > (richer than the `source_hash` form sketched in J5 above); a
  > `source_hash` column would need adding for staleness-based invalidation.
- **J7 -- `tur emit-c --output-dir` parity.** Same plumbing applies to
  `cmd_emit_c_to_dir`. Lower priority than `build`; should share
  J5/J6 code so the two entry points behave identically.

### G3 -- Fixtures + verification

- `module-spec-same-module/` -- generic def + concrete call in one
  module, built via `--output-dir`; the `.c` contains the clone body and
  calls it (J2).
- `module-spec-cross-module/` -- module A defines a generic; B and C
  each call it at the same concrete type. Exactly one clone body across
  the three `.c` files (owned by A); B and C reference it via A's
  header. Runtime output matches the whole-program build of the same
  sources (J3/J4/J6).
- `module-spec-cache-reuse/` -- build twice; second build reads
  `.tur-abi-cache/` and produces byte-identical `.c` output. Touching
  B's source does not change ownership of A's clone (J5).
- `--emit-abi-trace` over fixture 2 shows `concrete-clone` at B's and
  C's call sites, not `carrier`.

Aggregate signal: clone bodies appear once per `(fn, type_args)` across
the whole build; carriers appear only at genuinely polymorphic sites --
the same property the whole-program build already has, now preserved
under separate compilation.

---

## Theme H -- Tooling: `tur run` extended phases and `tur new` spec alignment

> Source: `tur-run-plan.md`. The basic `tur run` task runner is wired
> (`cmd_justrun` in `src/main.c:7559`; `tur run --init` and the
> Justfile template scaffold are referenced from
> `src/compiler/justrun.c:1351`). Per the user note in this plan's
> charter: "basic impl done; `tur new` and extended phases outstanding."
> `cmd_pkg_new` exists from PKG-1 but is **not** the
> `tur-run-plan` spec -- align the two before declaring NW done.

### H1 -- `tur run` extended phases

- **RN6 polish** -- `tur run --init` already exists; confirm it
  substitutes `{{ spice_name }}`, refuses to overwrite without
  `--force`, and writes exactly the supported-subset template (so the
  CI parity test in RN8 stays meaningful).
- **RN7 -- unsupported-feature diagnostics with line + column.** Every
  deferred Justfile construct (recipe attributes, `if`-expressions,
  modules, recipe groups, backticks, heredocs, aliases beyond the
  simple case, cross-recipe scoping) emits a specific error with line,
  column, and a one-line upgrade hint. Cover with `err-*` fixtures.
- **RN8 -- CI parity script.** `tools/just-vs-tur-run.sh` runs every
  `ok-*` fixture's recipes under both upstream `just` and `tur run`
  and asserts equal exit codes + identical stdout/stderr. Allow a soft
  skip on hosts without `just` installed. Wire as a GitHub Actions job
  in the main repo and in the spice-template workflow.
- **RN9 -- docs guide.** `docs/guides/tur-run-guide.md` already exists;
  confirm it documents the supported subset, the unsupported-feature
  table, exit codes, dotenv handling, the shell-choice / Windows
  story, and the `tur run` vs `tur <subcommand>` precedence rule
  (Risk #2 in the source plan).

### H2 -- `tur new` -- align `cmd_pkg_new` with the `tur-run-plan` spec

The source plan scopes `tur new` as a one-command scaffold producing the
RN6 Justfile, a buildable `src/`, a passing `tests/` file, a
`.gitignore`, optional `LICENSE`, optional `.github/workflows/ci.yml`,
and -- with `--no-git=false` -- a `git init` + initial commit.

The PKG-1 `cmd_pkg_new` predates this spec. Reconcile:

- Audit `cmd_pkg_new` output against the plan's "Generated layout"
  block. Track gaps in the order below.
- **NW0 -- validation.** *DONE (2026-05-30).* `valid_project_name`
  (`src/compiler/pkg.c`) now enforces the full rule: `[a-z][a-z0-9-]*`,
  2--64 chars, leads with a letter, and rejects the reserved names
  `tur` / `build` / `test` (`is_reserved_project_name`). All three
  `tur new` / `tur init` call sites share it, and their error messages
  spell out the complete rule.
- **NW1 -- templates.** Embed every scaffold file as a string
  constant. Substitutions resolved at scaffold time:
  `{{ spice_name }}`, `{{ author }}`, `{{ year }}`, `{{ license }}`.
  The Justfile template is **imported from** `tur/run/init`'s
  constant -- not copied. Unit test asserts byte-equality with the
  `tur run --init` template (Risk #3 in the source plan).
- **NW2 -- scaffold core.** Pure function `(opts) -> Vec[FileEntry
  {path, contents, mode}]` plus a separate writer. `--dry-run` prints
  the vec without writing. Refuse to write into a non-empty target
  unless `--here` was given over an empty dir.
- **NW3 -- git integration.** `git init` + `.gitignore` + initial
  commit ("Initial scaffold from `tur new`"). Honour `GIT_AUTHOR_*`
  env vars. Git failure is non-fatal -- print a one-line warning and
  continue.
- **NW4 -- CLI.** Flags: `--kind {lib,bin}` (default `lib`),
  `--author`, `--license {MIT,Apache-2.0,BSD-3-Clause,none}` (default
  `none`), `--no-git`, `--no-ci`, `--no-justfile`, `--dry-run`,
  `--here`. Defaults: author from `git config user.name` +
  `user.email`; fall back to `GIT_AUTHOR_*`, then a placeholder.
- **NW5 -- dispatch wiring.** Replace (or wrap) the `cmd_pkg_new`
  entry in `src/main.c:7656` with the new implementation. `tur new
  --help` documents every flag and the generated layout.
- **NW6 -- bootstrap CI test.** Scaffold a temp spice
  (`tur new tmp-spice`), `cd` in, run `tur run ci` (which runs
  `tur fmt --check`, `tur check`, `tur test`, `tur docs`), assert
  exit 0. **Acceptance gate** for the whole theme: until a freshly
  scaffolded spice passes its own CI recipe end-to-end, `tur new` is
  not done.

  > **Status (2026-05-30): BLOCKED -- larger than a test fixture.** A
  > freshly scaffolded spice does *not* pass `tur run ci` today, for
  > reasons that are prerequisites, not test wiring:
  >
  > 1. **The standard Justfile recipes invoke commands that don't work as
  >    written.** `build:`/`check:`/`test:` call `tur build` / `tur check`
  >    / `tur test` with no argument, but those subcommands require a
  >    target (a bare invocation prints usage and exits non-zero). They
  >    need `tur build .`, `tur check src/`, `tur test tests/`.
  >    (`tur check <dir>` now works -- this plan added directory-mode
  >    support; it previously crashed reading the directory as a file.)
  > 2. **`tur docs` is not a subcommand at all.** The `docs:` recipe and
  >    the `ci: clean check test docs` chain assume a `tur docs`
  >    generator that does not exist (only `tur doc <symbol>` lookup
  >    exists). NW6 as specified presumes `tur docs` ships first.
  > 3. **`tur fmt` strips `;;;` docstrings inside a `defmodule`** and
  >    collapses short forms onto one line, so any richly-documented
  >    scaffold fails `tur fmt --check` unless it is pre-collapsed. The
  >    scaffold source/test templates would have to be written in
  >    fmt-canonical form (module docstring only, no in-module defn
  >    docstrings), which is in tension with the docstring standard.
  >
  > Closing NW6 therefore requires: (a) a recipe overhaul in the shared
  > `JUSTFILE_TEMPLATE` (kept byte-identical between `justrun.c` and
  > `pkg.c` per NW1), (b) a real `tur docs` command or a redefinition of
  > the CI contract, and (c) fmt-clean scaffold templates. This is its
  > own change-set, tracked here rather than forced into a partial fix.
  >
  > **A dedicated, phased plan for this change-set now exists:**
  > [`docs/tur-new-ci-bootstrap-plan.md`](tur-new-ci-bootstrap-plan.md).
  > It re-verifies the blockers against HEAD (2026-05-30) -- including a
  > fourth one the original note missed: the scaffolded `src/` does not
  > even compile (`tur build .` fails on a `(str ...)` call that should be
  > `str-concat`) -- and sequences the fixes into four phases ending in the
  > NW6 bootstrap CI gate itself.
- **NW7 -- docs.** `docs/guides/tur-new-guide.md` already exists;
  confirm it covers the new flag set, the generated layout, and the
  "evolve a spice past the template" guidance from the source plan.

### H3 -- Repo-side follow-ups

- `turmeric-spices` contributor guide gets a "Standard recipes"
  section listing the template's recipes. Existing-spice maintainers
  run `tur run --init --force` after reviewing the diff to bring an
  older spice up to template parity.
- The `turmeric-spices` CI workflow gains a step that runs
  `tur run ci` from each spice directory, replacing the ad-hoc
  `cd spices/foo && just test` pattern. The CI job no longer needs
  `just` installed on the runner.

---

## Cross-theme dependencies and sequencing summary

```
A (Clang)  ──► makes B/C/D/E/F/G testable on macOS without -Wno-error
B (carrier bridge)  ──► prerequisite for C, D, E (struct lowering paths)
C (sized polish)    ──► independent within Theme C; gated by B for inline-C audit
D (struct ABI)      ──► gated by B; D1's pass_by_ptr feeds Theme A's Family B
E (arity cap)       ──► independent of all of the above; "~1 day" per source plan
F (closure mono)    ──► gated by B, D
G (cross-module)    ──► gated by F (and the whole-program pipeline A--F refines)
H (tooling)         ──► independent of compiler work; can land in parallel
```

Suggested batching for a small team / single-thread agent:

1. Land Theme A end-to-end. CI on macOS Clang becomes "real."
2. Land Theme B (Phase A of the unboxing plan).
3. Pick one of {C, D, E} per cycle. E is the smallest and most
   self-contained -- a good warm-up.
4. Theme F after D's `pass_by_ptr` decision is stable.
5. Theme G is large; partially landed already. Verify state, then close
   J2/J3/J4 first (correctness), J5/J6/J7 later (optimization).
6. Theme H is independent; pull it in whenever there's a pause in
   compiler work, or run it in parallel.

---

## Out of scope (carried over unchanged from source plans)

- Erasure-free generics (specializing every call) -- the carrier ABI
  exists precisely to avoid that blowup.
- GC / tagged-pointer reworks -- separate runtime plan.
- SIMD / vector primitives -- prerequisite is sized primitives, but
  the SIMD axis is its own design.
- Refinement types over sized primitives -- tracked by
  `docs/upcoming/refinement-types-plan.md`.
- Dependent sized types (lift the phantom `SizedVec` size into values)
  -- noted in sized-types memory; would build on Theme C but is not
  part of it.
- HList-style tuples -- Appendix A of the arbitrary-arity plan
  evaluates them on their own merits; Theme E does not block them.
- Backtick command substitution / recipe attributes / module imports
  in `tur run` -- v0.2 follow-ups.

---

## Open questions

All open questions in the source plans have been resolved:

- **Mixed-width arithmetic:** no implicit coercion (Theme C1).
- **Pass-by-pointer threshold:** 16 bytes (Theme D1).
- **Closure specialization cap:** none (Theme F).
- **Typeclass dispatch path:** (b) -- keep dictionaries, specialize at
  statically-known call sites. *Landed* (Unboxing Phase H).
- **Phase G inline-C:** template marker approach. *Landed* (Unboxing
  Phase G).
- **`tur new` scope:** in-scope companion to `tur run` / `tur fmt`,
  shipping in the same release (Theme H2).

---

## Verification

Per-theme signals are above. The aggregate signal for the roadmap is:

- The Clang-gated `-Wno-error` block is gone from `tests/run.sh` and
  `tur build` on macOS works with no special env (Theme A).
- KB-004/010/012/015 fixtures pass (Theme B).
- `numeric-types*` fixtures still pass; new `sized-*` fixtures land
  (Theme C).
- An `--emit-abi-trace` run on a representative project shows clones
  appearing once per `(fn, type_args)` across the whole build, with
  carriers only at genuinely polymorphic sites (Themes F + G).
- `docs/guides/type-erasure-guide.md` gets a "Status: post-Theme X"
  note and an updated map of which kinds still erase (Themes B/C/D).
- A freshly-scaffolded `tur new tmp-spice` passes its own `tur run ci`
  end-to-end (Theme H).
